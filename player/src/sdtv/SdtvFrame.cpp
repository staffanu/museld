// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <format>
#include <vector>
#include "SdtvFrame.h"
#include "SdtvFieldView.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/HalfFloatUtil.h"
#include "util/RobustNoise.h"

SdtvFrame::SdtvFrame(Logger &log, int frame_no, musevk::VulkanManager &manager, const VideoStandard &standard)
: m_log(log),
        m_standard(standard),
        m_frame_no(frame_no),
        m_input_offset(-1),
        m_input_samples_per_sample(0),
        m_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(standard.samples_per_line, standard.total_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostRead)),
        m_burst_phase_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(standard.total_lines), 4 /* 2 * sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_dropout_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(standard.samples_per_line, standard.total_lines), sizeof(uint8_t),
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_fields({SdtvFieldView(log, frame_no, m_data, m_burst_phase_data, m_dropout_data, 0),
                  SdtvFieldView(log, frame_no, m_data, m_burst_phase_data, m_dropout_data, 1) }) {
}

SdtvFrame::NoiseEstimate SdtvFrame::EstimateNoise(float const *data, const VideoStandard &standard) {
    // Back porch windows sit after the colour burst and before active video
    // (NTSC: the burst reaches ~column 112, the picture starts at 129); sync
    // tip windows inside the horizontal sync pulse (67 samples on NTSC; on
    // PAL only the last 0.5 us of the 83, after the 3.75 MHz pilot burst
    // that rides on the tip).  211 rows per field from noise_rows_start
    // (NTSC: 40-250 and 303-513) keep clear of vertical sync and the VBI
    // code lines (white flag, picture numbers).
    const int width = standard.samples_per_line;
    NoiseEstimate est{};
    std::vector<float> porch_residuals, sync_residuals, centers;
    porch_residuals.reserve(422 * 16);
    sync_residuals.reserve(422 * standard.noise_sync_len);
    centers.reserve(422);
    for (int field_start : standard.noise_rows_start) {
        for (int row = field_start; row <= field_start + 210; row++) {
            float center;
            RobustNoise::appendDetrendedResiduals(data + row * width + standard.noise_porch_col, 16, porch_residuals, &center);
            centers.push_back(center);
            if (standard.noise_sync_len > 0)
                RobustNoise::appendDetrendedResiduals(data + row * width + standard.noise_sync_col,
                                                      standard.noise_sync_len, sync_residuals);
        }
    }
    est.sigma_blanking = RobustNoise::robustSigma(porch_residuals);
    est.sigma_sync = sync_residuals.empty() ? -1.0f : RobustNoise::robustSigma(sync_residuals);
    est.blanking_level = RobustNoise::median(centers);

    // White flag: a full flat line at 100 IRE in the vertical interval (IEC
    // 60857 writes it on line 11/274 to mark the first field of a film frame,
    // but mastering varies, so scan the same candidate rows as the noise
    // spectrum).  A window qualifies when it is flat (rejects Philips code
    // pulses, which detrend to σ ≈ 0.5) and sits at the nominal 0.7 V above
    // the blanking level just measured (rejects blank lines and captions).
    // It is the only trustworthy gain reference on this medium: sync depth
    // measures ~14 % off its 40 IRE definition on real captures.  PAL discs
    // carry no white flag.
    std::vector<float> white_centers;
    float sigma_gate = std::max(3.0f * est.sigma_blanking, 0.02f);
    if (standard.has_white_flag) {
        for (int row : standard.blank_vbi_lines) {
            float row_centers[2];
            bool qualified = true;
            for (int w = 0; w < 2; w++) {
                std::vector<float> residuals;
                RobustNoise::appendDetrendedResiduals(
                        data + row * width + standard.noise_psd_cols[w], 256, residuals, &row_centers[w]);
                if (std::abs(row_centers[w] - est.blanking_level - 0.7f) > 0.15f ||
                    RobustNoise::robustSigma(residuals) > sigma_gate)
                    qualified = false;
            }
            if (qualified && std::abs(row_centers[0] - row_centers[1]) < 0.05f)
                white_centers.push_back(0.5f * (row_centers[0] + row_centers[1]));
        }
    }
    est.white_flag_level = white_centers.empty() ? -1.0f : RobustNoise::median(white_centers);

    // Luma histogram: means over 4 samples (one subcarrier cycle on the
    // 4 fsc grid, so the chroma cancels) across the active picture of the
    // same rows, a microsecond clear of both blanking edges
    est.luma_hist.fill(0);
    for (int field_start : standard.noise_rows_start) {
        for (int row = field_start; row <= field_start + 210; row++) {
            const float *line = data + row * width;
            for (int col = standard.luma_hist_col0; col + 4 <= standard.luma_hist_col1; col += 4) {
                const float mean = 0.25f * (line[col] + line[col + 1] + line[col + 2] + line[col + 3]);
                const int bin = (int)std::floor((mean - est.blanking_level - NoiseEstimate::c_luma_hist_min)
                                                / NoiseEstimate::c_luma_hist_bin);
                if (bin >= 0 && bin < NoiseEstimate::c_luma_hist_bins)
                    est.luma_hist[bin]++;
            }
        }
    }

    // Burst phase: correlate the colour burst window (columns 78..110, 8
    // subcarrier cycles on the 4 fsc grid) against the quadrature pair per
    // line.  The burst inverts line to line, so odd lines are flipped before
    // the amplitude-weighted circular statistics.  NTSC only: PAL's burst
    // swings +-45 degrees and walks against the line-locked grid, so this
    // statistic means nothing there (the PAL colour decoder is to come).
    est.burst_phase = 0;
    est.burst_phase_sigma = 0;
    if (standard.ntsc_chroma) {
        static constexpr float lut_cos[4] = {1, 0, -1, 0};
        static constexpr float lut_sin[4] = {0, 1, 0, -1};
        std::vector<std::pair<float, float>> line_vecs;
        line_vecs.reserve(422);
        double sum_i = 0, sum_q = 0;
        for (int field_start : standard.noise_rows_start) {
            for (int row = field_start; row <= field_start + 210; row++) {
                float bi = 0, bq = 0;
                for (int x = 78; x < 110; x++) {
                    float v = data[row * width + x];
                    bi += v * lut_cos[x % 4];
                    bq += v * lut_sin[x % 4];
                }
                if (row & 1) { bi = -bi; bq = -bq; }
                line_vecs.emplace_back(bi, bq);
                sum_i += bi;
                sum_q += bq;
            }
        }
        est.burst_phase = (float)atan2(sum_q, sum_i);
        double var_sum = 0, w_sum = 0;
        for (auto [bi, bq] : line_vecs) {
            double a = std::hypot(bi, bq);
            if (a <= 0)
                continue;
            double d = std::remainder(atan2(bq, bi) - est.burst_phase, 2 * M_PI);
            var_sum += a * d * d;
            w_sum += a;
        }
        est.burst_phase_sigma = w_sum > 0 ? (float)sqrt(var_sum / w_sum) : 0.0f;
    }
    return est;
}

int SdtvFrame::AccumulateNoisePsd(float const *data, double *psd, float max_sigma, const VideoStandard &standard) {
    // Candidate blank VBI rows.  Discs differ in which lines carry the white
    // flag, picture numbers, and captions, so every window must qualify
    // instead: level close to blanking (rejects the white flag and active
    // video) and sigma below the gate (a Philips code line measures ~0.5).
    const int width = standard.samples_per_line;
    int windows = 0;
    for (int row : standard.blank_vbi_lines)
        for (int col : standard.noise_psd_cols) {
            std::vector<float> residuals;
            float center;
            RobustNoise::appendDetrendedResiduals(data + row * width + col, 256, residuals, &center);
            if (std::abs(center - 0.3f) > 0.1f || RobustNoise::robustSigma(residuals) > max_sigma)
                continue;
            RobustNoise::accumulateDetrendedWindowPsd(data + row * width + col, 256, psd);
            windows++;
        }
    return windows;
}

void SdtvFrame::set_frame_no(int frame_no, int64_t input_offset, double input_samples_per_sample) {
    m_frame_no = frame_no;
    m_input_offset = input_offset;
    m_input_samples_per_sample = input_samples_per_sample;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

int64_t SdtvFrame::getInputOffset() const {
    return m_input_offset;
}

double SdtvFrame::getInputSamplesPerSdtvSample() const {
    return m_input_samples_per_sample;
}

std::shared_ptr<musevk::VulkanBuffer> &SdtvFrame::data() {
    return m_data;
}

std::shared_ptr<musevk::VulkanBuffer> &SdtvFrame::dropout_data() {
    return m_dropout_data;
}

std::shared_ptr<musevk::VulkanBuffer> &SdtvFrame::burst_phase_data() {
    return m_burst_phase_data;
}


SdtvFieldView &SdtvFrame::get_field(int parity) {
    return m_fields[parity];
}

std::shared_ptr<VbiData> SdtvFrame::getVbiData() const {
    return m_vbi_data;
}

std::optional<std::pair<uint8_t, uint8_t>> SdtvFrame::getClosedCaptionBytes() const {
    return m_cc_bytes;
}

void SdtvFrame::processVbi() {
    m_vbi_data = nullptr;

    // EIA-608 closed captions ride on line 21 of field 1 (the caption
    // services of field 2, line 284, carry CC3/CC4 and XDS and are not
    // decoded).  NTSC only.
    m_cc_bytes = m_standard.has_closed_captions ? processCcLine(21) : std::nullopt;

    // The Philips VBI codes appear on lines 16/17/18 of each field, and this
    // frame's buffer holds the two fields back to back: field 1 from line 1 and
    // field 2 from line 264 (314 on PAL), so field 2's code lines are
    // 279/280/281 (329-331).
    //
    // That layout holds only because SdtvFrameReader identifies the field from
    // the phase of the vertical sync pattern and always starts a frame on field
    // 1; a frame started on field 2 would put the second field's codes one line
    // earlier.  Rather than depend on that, we probe every candidate line and
    // identify each code by its content — the IEC 60857 §10.1 codes all have
    // distinct nibble patterns, and nothing downstream needs to know which field
    // a code came from.  Probing 278 as well covers a frame that starts on the
    // wrong field, so a lock that slips cannot turn a CLV disc into a CAV one by
    // losing the 87FFFF marker.
    std::vector<int> codes;
    for (int line : m_standard.vbi_code_lines) {
        int code = processVbiLine(line);
        if (code >= 0)
            codes.push_back(code);
    }

    // How many of the probed lines carry exactly this code, and the first line
    // matching a mask/pattern (-1 when absent).
    auto count = [&codes](int code) {
        return static_cast<int>(std::count(codes.begin(), codes.end(), code));
    };
    auto find = [&codes](int mask, int pattern) {
        auto it = std::find_if(codes.begin(), codes.end(),
            [mask, pattern](int code) { return (code & mask) == pattern; });
        return it == codes.end() ? -1 : *it;
    };
    // The five BCD digits of a picture number must all be 0-9.  This is what
    // separates a CAV picture number (§10.1.4) from a CLV programme time code
    // (§10.1.6): both start with an F, but the time code's "DD" is not BCD.
    auto isBcdPictureNumber = [](int code) {
        if ((code & 0xf00000) != 0xf00000) return false;
        for (int shift = 0; shift < 20; shift += 4)
            if (((code >> shift) & 0xf) > 9) return false;
        return true;
    };

    // Lead-in/out sit on lines 17/18 of both fields, so all four probes agree;
    // accept three to survive a dropout on one line.
    int is_lead_in = count(0x88FFFF) >= 3;
    int is_lead_out = count(0x80EEEE) >= 3;

    // CLV is flagged by the single-line 87FFFF CLV code (§10.1.7), always
    // present on a CLV disc, on the field that does NOT carry the programme
    // time code.
    // The programme time code (F0DDxx) is itself CLV-only, so it proves CLV
    // when the marker line failed to slice this frame.
    int is_clv = count(0x87ffff) >= 1 || find(0xf0ff00, 0xf0dd00) != -1;
    // The stop code (§10.1.11) is on lines 16 and 17 of one field.
    bool is_stop_code = count(0x82CFFF) >= 2;

    int clv_time_data = is_clv ? find(0xf0ff00, 0xf0dd00) : -1;
    int chapter_data = find(0xf00fff, 0x800ddd);
    int programme_status_data = -1;
    if (int cx_on = find(0xfff000, 0x8dc000); cx_on != -1)
        programme_status_data = cx_on;
    else if (int cx_off = find(0xfff000, 0x8ba000); cx_off != -1)
        programme_status_data = cx_off;

    // Only four bits of the picture number are fixed, so unlike the other codes
    // it is not really validated by its pattern; require the two lines that
    // carry it (17 and 18 of the field) to agree, as they always do on a clean
    // read.  Both are inside the probe set under either alignment.
    std::optional<int> cav_picture_number = std::nullopt;
    if (!is_clv) {
        auto it = std::find_if(codes.begin(), codes.end(), [&](int code) {
            return isBcdPictureNumber(code) && count(code) >= 2;
        });
        if (it != codes.end())
            cav_picture_number = ((*it & 0xf0000) >> 16) * 10000 + ((*it & 0xf000) >> 12) * 1000 +
                ((*it & 0xf00) >> 8) * 100 + ((*it & 0xf0) >> 4) * 10 + (*it & 0xf);
    }


    // Chapter number "8 X1 X2 D D D" (§10.1.8): the top bit of X1 is a flag,
    // set over the first 400 frames of a chapter, not part of the number
    std::optional<int> chapter = std::nullopt;
    if (chapter_data != -1) {
        chapter = (chapter_data & 0xf00fff) == 0x800ddd ?
        std::make_optional((((chapter_data & 0xf0000) >> 16) & 7) * 10 + ((chapter_data & 0xf000) >> 12)) : std::nullopt;
    }

    std::optional<int> clv_time_seconds = std::nullopt;
    std::optional<int> clv_picture_number = std::nullopt;
    if (is_clv) {
        int time_seconds_tmp = (clv_time_data & 0xf0ff00) == 0xf0dd00 ?
            ((clv_time_data & 0xf0000) >> 16) * 3600 + ((clv_time_data & 0xf0) >> 4) * 600 + (clv_time_data & 0xf) * 60 : -1;

        // CLV picture number (IEC 60857 §10.1.10): "8 X1 E X3 X4 X5", where the
        // E in the third nibble distinguishes it from the users code ("...D...",
        // §10.1.9).  X1/X3 carry the seconds; without this the time has only the
        // hours/minutes from the programme time code, i.e. seconds stuck at :00.
        int clv_picture_number_data = find(0xf0f000, 0x80e000);

        if (clv_picture_number_data != -1) {
            int second = (((clv_picture_number_data & 0xf0000) >> 16) - 10) * 10 + ((clv_picture_number_data & 0xf00) >> 8);
            int picture_in_second = ((clv_picture_number_data & 0xf0) >> 4) * 10 + (clv_picture_number_data & 0xf);
            clv_picture_number = std::make_optional(picture_in_second);
            if (time_seconds_tmp != -1)
                time_seconds_tmp += second;
        }
        if (time_seconds_tmp != -1)
            clv_time_seconds = std::make_optional(time_seconds_tmp);
    }

    std::optional<bool> cx_enabled = std::nullopt;
    if ((programme_status_data & 0xfff000) == 0x8dc000)
        cx_enabled = std::make_optional(true);
    else if ((programme_status_data & 0xfff000) == 0x8ba000) {
        cx_enabled = std::make_optional(false);
    }
    std::optional<bool> eight_inch = std::nullopt;
    std::optional<int> disk_side = std::nullopt;
    std::optional<bool> has_teletext = std::nullopt;
    std::optional<bool> audio_is_fm_fm_multiplex = std::nullopt;
    std::optional<bool> digital_video = std::nullopt;
    if (cx_enabled.has_value()) {
        int x3 = (programme_status_data & 0xf00) >> 8;
        int x4 = (programme_status_data & 0xf0) >> 4;
        int x5 = programme_status_data & 0xf;

        eight_inch = std::make_optional((x3 & 8) != 0);
        disk_side = std::make_optional((x3 & 4) != 0 ? 2 : 1);
        has_teletext = std::make_optional((x3 & 2) != 0);
        audio_is_fm_fm_multiplex = std::make_optional((x3 & 1) != 0);
        digital_video = std::make_optional((x4 & 4) != 0);

        bool x41 = (x4 & 8) != 0;
        bool x42 = (x4 & 4) != 0;
        bool x43 = (x4 & 2) != 0;
        bool x44 = (x4 & 1) != 0;
        bool x51 = (x5 & 8) != 0;
        bool x52 = (x5 & 4) != 0;
        bool x53 = (x5 & 2) != 0;
        if (x41 ^ x42 ^ x44 ^ x51 || x41 ^ x43 ^ x44 ^ x52 || x42 ^ x43 ^ x44 ^ x53)
            m_log.debug(eDecoder, "VBI programme status: X4/X5 parity error");
    }

    if (m_log.isEnabled(eDebug, eDecoder)) {
        std::string hex;
        for (int code : codes)
            hex += std::format(" {:06X}", code);
        m_log.debug(eDecoder, std::format("VBI frame {}: codes{}; clv {} time {} pic {} chapter {} cx {}",
                                          m_frame_no, hex, is_clv,
                                          clv_time_seconds ? std::to_string(*clv_time_seconds) : "-",
                                          clv_picture_number ? std::to_string(*clv_picture_number) : "-",
                                          chapter ? std::to_string(*chapter) : "-",
                                          cx_enabled ? (*cx_enabled ? "on" : "off") : "-"));
    }
    m_vbi_data = std::make_shared<VbiData>(is_lead_in, is_lead_out, is_clv, is_stop_code, chapter,
        clv_time_seconds, clv_picture_number, cav_picture_number, cx_enabled, m_standard.framesPerSecond());

    if (m_log.isEnabled(eDebug, eDecoder)) {
        auto strings = m_vbi_data->asStrings();
        std::string code_list;
        for (int code : codes)
            code_list += std::format("{:06x} ", code & 0xffffff);
        m_log.debug(eDecoder, std::format("VBI {} {}|{}", code_list, strings[0], strings[1]));
    }
}

// Notice line starts at 1
int SdtvFrame::processVbiLine(int line) {
    // The code starts at ~0.172 H (spec: 0.172 H or 0.188 H).  Sample 0 is at
    // 0H within a sample or two (measured from the colour burst, which starts
    // ~0.083 H; the sync tip itself is clamped out of this buffer), so the code's
    // rising edge really does land near 0.166 H here — a hair earlier on some
    // discs.  We begin the search at 0.15 H, in clean back porch: the burst ends
    // ~0.123 H and the first code bit rises ~0.166 H, so 0.15 H sits between them
    // (~24 samples clear of the burst).  0.165 H was too late — it coincided with
    // the code's own rising edge and failed the gate (alien1 lines 16/17 — the
    // programme status and CLV marker).
    const int width = m_standard.samples_per_line;
    int16_t *vbi = m_data->data<int16_t>() + (line - 1) * width + (int)(0.15 * width);

    if (HalfFloatUtil::half_to_float(vbi[0]) > 0.5) {
        m_log.debug(eDecoder, std::format("VBI line {}: signal present before code start", line));
        return -1;
    }

    for (int i = 0; i < (int)(0.005e-3 * m_standard.sampling_frequency); i++) {
        if (HalfFloatUtil::half_to_float(*vbi) > 0.7)
            goto found_start;
        vbi++;
    }
    //printf("VBI error: can not find start\n");
    return -1;

    found_start:

    int samples_per_half_bit = 1e-6 * m_standard.sampling_frequency;
    long half_bits = 0b01; // first two half bits are 01, already found
    int prev = 1;
    // A half bit is 14 samples, so a one- or two-sample dip or spike (a
    // noise hit, a speck on the disc) is not a transition: slice the
    // 3-sample median, with hysteresis around the half-amplitude level
    auto median3 = [](const int16_t *p) {
        const float a = HalfFloatUtil::half_to_float(p[-1]);
        const float b = HalfFloatUtil::half_to_float(p[0]);
        const float c = HalfFloatUtil::half_to_float(p[1]);
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    };
    for (int i = 2; i < 48; i++) {
        // measure time to next transition
        int t = 0;
        while (t < samples_per_half_bit * 9 / 4) {
            float v = median3(vbi++);
            if (prev && v < 0.35f || !prev && v > 0.65f) {
                prev = 1 - prev;
                break;
            }
            t++;
        }

        if (t < samples_per_half_bit * 3 / 4) {
            m_log.debug(eDecoder, std::format("VBI line {}: transition time too short", line));
            // Debug aid: MUSELD_DUMP_VBI_FAIL=<path> appends the failing line (samples_per_line floats)
            if (static const char *dump = getenv("MUSELD_DUMP_VBI_FAIL"); dump != nullptr) {
                if (FILE *f = fopen(dump, "ab")) {
                    const int16_t *row = m_data->data<int16_t>() + (line - 1) * width;
                    for (int j = 0; j < width; j++) {
                        float v = HalfFloatUtil::half_to_float(row[j]);
                        fwrite(&v, sizeof v, 1, f);
                    }
                    fclose(f);
                }
            }
            return -1;
        } else if (t < samples_per_half_bit * 5 / 4) {
            half_bits = (half_bits << 1) | prev;
        } else if (t < samples_per_half_bit * 7 / 4) {
            m_log.debug(eDecoder, std::format("VBI line {}: transition time not one or two half bits", line));
            return -1;
        } else if (t < samples_per_half_bit * 9 / 4) {
            half_bits = (half_bits << 2) | ((1 - prev) << 1) | prev;
            i++;
        } else {
            m_log.debug(eDecoder, std::format("VBI line {}: transition time too long", line));
            return -1;
        }
    }
    int codeword = 0;
    for (int i = 0; i < 24; i++) {
        int two_half_bits = (half_bits >> (46 - 2 * i)) & 0b11;
        codeword <<= 1;
        switch (two_half_bits) {
            case 0b01:
                codeword |= 1;
                break;
            case 0b10:
                break;
            default:
                m_log.debug(eDecoder, std::format("VBI line {}: invalid two-bit pair at bit {}: {:x}", line, i, half_bits));
                return -1;
        }
    }
    return codeword;
}

// EIA-608 waveform (CEA-608-E §8.1): a clock run-in of 7 sine cycles at 32 x fH
// (0.5035 MHz) starting 10.5 µs after 0H, two zero bits, a '1' start bit, then
// 16 NRZ data bits, LSB first, at the same rate.  0 is the blanking level, 1 is
// 50 IRE.  Returns the two bytes with their (odd) parity bits intact, or
// nullopt when no caption waveform is found on the line.
std::optional<std::pair<uint8_t, uint8_t>> SdtvFrame::processCcLine(int line) {
    const int width = m_standard.samples_per_line;
    const double T = width / 32.0; // one 608 clock period in samples
    const int16_t *row = m_data->data<int16_t>() + (line - 1) * width;
    auto sample = [row](int i) { return HalfFloatUtil::half_to_float(row[i]); };

    // Slicing level: halfway between blanking, measured on the back porch
    // (after the colour burst, before the run-in), and the 50 IRE data level.
    float zero = 0;
    for (int i = 113; i < 129; i++)
        zero += sample(i);
    zero /= 16;
    const float threshold = zero + 0.175f;

    // The next upward crossing of the slicing level at or after `from`,
    // linearly interpolated; -1 when none is found before `to`.
    auto nextRise = [&](double from, double to) -> double {
        for (int i = std::max((int)from, 1); i < std::min((int)to, width - 1); i++) {
            float a = sample(i - 1), b = sample(i);
            if (a <= threshold && b > threshold)
                return i - 1 + (threshold - a) / (b - a);
        }
        return -1;
    };

    // The run-in's first upward crossing sits a quarter period past its
    // nominal 10.5 µs start; search a window generously bracketing that.
    const double t0 = nextRise(0.14 * width, 0.25 * width);
    if (t0 < 0)
        return std::nullopt; // blank line: no caption service

    // Validate the run-in: one upward crossing per clock period for the
    // remaining six cycles, each near its expected position.
    double t = t0;
    for (int k = 1; k <= 6; k++) {
        t = nextRise(t + 0.5 * T, t0 + k * T + 0.5 * T);
        if (t < 0 || std::abs(t - (t0 + k * T)) > 0.35 * T) {
            m_log.debug(eDecoder, std::format("CC line {}: clock run-in cycle {} misplaced", line, k));
            return std::nullopt;
        }
    }

    // The '1' start bit rises two zero bits after the run-in's 7 cycles: at
    // 9 periods past the run-in start, i.e. 8.75 T past the first crossing.
    const double edge = nextRise(t0 + 7.4 * T, t0 + 9.6 * T);
    if (edge < 0 || std::abs(edge - (t0 + 8.75 * T)) > 0.5 * T) {
        m_log.debug(eDecoder, std::format("CC line {}: start bit not found", line));
        return std::nullopt;
    }

    // Sample the 16 data bits at their centers, 1.5 periods past the start
    // bit's leading edge onward
    uint16_t bits = 0;
    for (int k = 0; k < 16; k++) {
        const int center = (int)std::lround(edge + (1.5 + k) * T);
        float v = 0;
        for (int i = center - 2; i <= center + 2; i++)
            v += sample(std::min(i, width - 1));
        if (v / 5 > threshold)
            bits |= (uint16_t)(1 << k);
    }
    return std::make_pair((uint8_t)(bits & 0xff), (uint8_t)(bits >> 8));
}
