// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// Reference VIT mono-pulse for the MUSE adaptive equalizer.
//
// The target end-to-end response is a raised-cosine pulse with roll-off factor β = 0.1
// and the −6 dB point at 8.1 MHz (= 16.2 MHz / 2).  Two 16.2 MHz per-C-phase references
// are exposed:
//
//   • onSamplePulse() for frames whose underlying continuous-time pulse peak coincides
//     with the 16.2 MHz sample raster (= a Kronecker delta at c_center, since the
//     raised cosine satisfies the Nyquist criterion at the symbol rate).
//   • offSamplePulse() for frames where the pulse is shifted half a sample to the
//     left, so the raster captures the impulse response at half-integer T offsets
//     (= a spread shape with ~±0.635·peak at the two samples bracketing the
//     between-sample peak).

#ifndef MUSECPP_MUSEEQREFERENCE_H
#define MUSECPP_MUSEEQREFERENCE_H

#include <array>
#include <cmath>

#include "FrameBuffer.h"

class MuseEqReference {
public:
    static constexpr float c_beta = 0.1f;
    static constexpr int c_length = FrameBuffer::c_vits_sample_count;
    static constexpr int c_center = FrameBuffer::c_vits_pulse_offset;

    static const std::array<float, c_length> &onSamplePulse() {
        static const std::array<float, c_length> table = build(0.0f);
        return table;
    }
    static const std::array<float, c_length> &offSamplePulse() {
        // The phase-1 underlying pulse is half a sample earlier than phase-0's peak; in
        // the sampling grid centred on c_center our sample at offset n is therefore at
        // (n − c_center + 0.5) sample periods from the actual pulse peak.
        static const std::array<float, c_length> table = build(0.5f);
        return table;
    }

private:
    // Raised-cosine impulse response sampled at t (in MUSE sample periods).
    static float raisedCosine(float t) {
        const float pi = 3.14159265358979323846f;
        float sinc = (t == 0.0f) ? 1.0f : std::sin(pi * t) / (pi * t);
        float denom = 1.0f - 4.0f * c_beta * c_beta * t * t;
        float cos_term;
        if (std::abs(denom) < 1e-6f) {
            // Removable singularity at t = ±1/(2β); L'Hôpital gives (π/4) sin(π/(2β))
            cos_term = (pi / 4.0f) * std::sin(pi / (2.0f * c_beta));
        } else {
            cos_term = std::cos(pi * c_beta * t) / denom;
        }
        return sinc * cos_term;
    }

    static std::array<float, c_length> build(float pulse_shift) {
        std::array<float, c_length> h{};
        for (int n = 0; n < c_length; n++) {
            // pulse_shift = 0 for on-sample, 0.5 for off-sample (half-sample left).
            float t = (float)(n - c_center) + pulse_shift;
            h[n] = raisedCosine(t);
        }
        return h;
    }
};

#endif //MUSECPP_MUSEEQREFERENCE_H
