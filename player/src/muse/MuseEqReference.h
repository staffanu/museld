// Reference VIT mono-pulse for the MUSE adaptive equalizer.
//
// The target end-to-end response is a raised-cosine pulse with roll-off factor β = 0.1
// and the −6 dB point at 8.1 MHz (= 16.2 MHz / 2).  Sampled at 32.4 MHz this gives
// two samples per MUSE pixel — the rate at which we observe the channel by combining
// VITS from two consecutive frames.

#ifndef MUSECPP_MUSEEQREFERENCE_H
#define MUSECPP_MUSEEQREFERENCE_H

#include <array>
#include <cmath>

#include "FrameBuffer.h"

class MuseEqReference {
public:
    // 32.4 MHz reference: 100 slots covering 50 useful 16.2 MHz samples from each phase.
    // (We extract 51 16.2 MHz samples per frame; phase-0 uses offsets 1..50 and phase-1
    // uses offsets 0..49 — see MuseAdaptiveEqualizer::runLmsStep.)
    static constexpr int c_length = 2 * (FrameBuffer::c_vits_sample_count - 1);
    // Phase-0 frame samples occupy odd 32.4 MHz slots; phase-1 occupies even slots one
    // 32.4 MHz cycle earlier in phase-0's reference grid.  The phase-0 pulse peak (at
    // extracted offset c_vits_pulse_offset = 25) lands at 32.4 MHz slot 2*(25-1)+1 = 49.
    static constexpr int c_center = 2 * (FrameBuffer::c_vits_pulse_offset - 1) + 1;

    // Roll-off parameters (raised cosine with β=0.1, −6 dB at 8.1 MHz).
    // t is measured in MUSE sample periods (1 / 16.2 MHz); at 32.4 MHz sampling
    // each step is 0.5 MUSE sample periods.
    static constexpr float c_beta = 0.1f;

    // Returns the reference pulse h[n] centered at n = c_center.  Lazily built on
    // first call; subsequent calls return the same buffer.
    static const std::array<float, c_length> &pulse() {
        static const std::array<float, c_length> table = build();
        return table;
    }

private:
    static std::array<float, c_length> build() {
        std::array<float, c_length> h{};
        const float pi = 3.14159265358979323846f;
        for (int n = 0; n < c_length; n++) {
            float t = (float)(n - c_center) * 0.5f; // in MUSE sample periods
            float sinc = (t == 0.0f) ? 1.0f : std::sin(pi * t) / (pi * t);
            float denom = 1.0f - 4.0f * c_beta * c_beta * t * t;
            float cos_term;
            if (std::abs(denom) < 1e-6f) {
                // Removable singularity at t = ±1/(2β); L'Hôpital gives (π/4) sin(π/(2β))
                cos_term = (pi / 4.0f) * std::sin(pi / (2.0f * c_beta));
            } else {
                cos_term = std::cos(pi * c_beta * t) / denom;
            }
            h[n] = sinc * cos_term;
        }
        return h;
    }
};

#endif //MUSECPP_MUSEEQREFERENCE_H
