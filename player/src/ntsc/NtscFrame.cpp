// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <algorithm>
#include <format>
#include <vector>
#include "NtscFrame.h"
#include "NtscConstants.h"
#include "NtscFieldView.h"
#include "NtscInputBlock.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/HalfFloatUtil.h"
#include "util/RobustNoise.h"

NtscFrame::NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager)
: m_log(log),
        m_frame_no(frame_no),
        m_input_offset(-1),
        m_input_samples_per_sample(0),
        m_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostRead)),
        m_burst_phase_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_total_video_lines), 4 /* 2 * sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_dropout_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), sizeof(uint8_t),
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_fields({NtscFieldView(log, frame_no, m_data, m_burst_phase_data, m_dropout_data, 0),
                  NtscFieldView(log, frame_no, m_data, m_burst_phase_data, m_dropout_data, 1) }) {
}

NtscFrame::NoiseEstimate NtscFrame::EstimateNoise(float const *data) {
    // Back porch windows sit after the colour burst (which reaches ~column 112)
    // and before active video at column NTSC_FIELD_START_X = 129; sync tip
    // windows inside the ~67-sample horizontal sync pulse.  Rows 40-250 and
    // 303-513 keep clear of vertical sync and the VBI code lines (white flag,
    // picture numbers).
    NoiseEstimate est{};
    std::vector<float> porch_residuals, sync_residuals, centers;
    porch_residuals.reserve(422 * 16);
    sync_residuals.reserve(422 * 48);
    centers.reserve(422);
    for (int field_start : {40, 303}) {
        for (int row = field_start; row <= field_start + 210; row++) {
            float center;
            RobustNoise::appendDetrendedResiduals(data + row * NTSC_TOTAL_WIDTH + 113, 16, porch_residuals, &center);
            centers.push_back(center);
            RobustNoise::appendDetrendedResiduals(data + row * NTSC_TOTAL_WIDTH + 8, 48, sync_residuals);
        }
    }
    est.sigma_blanking = RobustNoise::robustSigma(porch_residuals);
    est.sigma_sync = RobustNoise::robustSigma(sync_residuals);
    est.blanking_level = RobustNoise::median(centers);

    // White flag: a full flat line at 100 IRE in the vertical interval (IEC
    // 60857 writes it on line 11/274 to mark the first field of a film frame,
    // but mastering varies, so scan the same candidate rows as the noise
    // spectrum).  A window qualifies when it is flat (rejects Philips code
    // pulses, which detrend to σ ≈ 0.5) and sits at the nominal 0.7 V above
    // the blanking level just measured (rejects blank lines and captions).
    // It is the only trustworthy gain reference on this medium: sync depth
    // measures ~14 % off its 40 IRE definition on real captures.
    std::vector<float> white_centers;
    float sigma_gate = std::max(3.0f * est.sigma_blanking, 0.02f);
    for (int row : {8, 10, 11, 12, 13, 14, 270, 272, 273, 274, 275, 276}) {
        float row_centers[2];
        bool qualified = true;
        for (int w = 0; w < 2; w++) {
            std::vector<float> residuals;
            RobustNoise::appendDetrendedResiduals(
                    data + row * NTSC_TOTAL_WIDTH + (w == 0 ? 150 : 425), 256, residuals, &row_centers[w]);
            if (std::abs(row_centers[w] - est.blanking_level - 0.7f) > 0.15f ||
                RobustNoise::robustSigma(residuals) > sigma_gate)
                qualified = false;
        }
        if (qualified && std::abs(row_centers[0] - row_centers[1]) < 0.05f)
            white_centers.push_back(0.5f * (row_centers[0] + row_centers[1]));
    }
    est.white_flag_level = white_centers.empty() ? -1.0f : RobustNoise::median(white_centers);

    // Burst phase: correlate the colour burst window (columns 78..110, 8
    // subcarrier cycles on the 4 fsc grid) against the quadrature pair per
    // line.  The burst inverts line to line, so odd lines are flipped before
    // the amplitude-weighted circular statistics.
    {
        static constexpr float lut_cos[4] = {1, 0, -1, 0};
        static constexpr float lut_sin[4] = {0, 1, 0, -1};
        std::vector<std::pair<float, float>> line_vecs;
        line_vecs.reserve(422);
        double sum_i = 0, sum_q = 0;
        for (int field_start : {40, 303}) {
            for (int row = field_start; row <= field_start + 210; row++) {
                float bi = 0, bq = 0;
                for (int x = 78; x < 110; x++) {
                    float v = data[row * NTSC_TOTAL_WIDTH + x];
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

int NtscFrame::AccumulateNoisePsd(float const *data, double *psd, float max_sigma) {
    // Candidate blank VBI rows.  Discs differ in which lines carry the white
    // flag, picture numbers, and captions, so every window must qualify
    // instead: level close to blanking (rejects the white flag and active
    // video) and sigma below the gate (a Philips code line measures ~0.5).
    int windows = 0;
    for (int row : {8, 10, 11, 12, 13, 14, 270, 272, 273, 274, 275, 276})
        for (int col : {150, 425}) {
            std::vector<float> residuals;
            float center;
            RobustNoise::appendDetrendedResiduals(data + row * NTSC_TOTAL_WIDTH + col, 256, residuals, &center);
            if (std::abs(center - 0.3f) > 0.1f || RobustNoise::robustSigma(residuals) > max_sigma)
                continue;
            RobustNoise::accumulateDetrendedWindowPsd(data + row * NTSC_TOTAL_WIDTH + col, 256, psd);
            windows++;
        }
    return windows;
}

void NtscFrame::set_frame_no(int frame_no, int64_t input_offset, double input_samples_per_sample) {
    m_frame_no = frame_no;
    m_input_offset = input_offset;
    m_input_samples_per_sample = input_samples_per_sample;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

int64_t NtscFrame::getInputOffset() const {
    return m_input_offset;
}

double NtscFrame::getInputSamplesPerNtscSample() const {
    return m_input_samples_per_sample;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::data() {
    return m_data;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::dropout_data() {
    return m_dropout_data;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::burst_phase_data() {
    return m_burst_phase_data;
}


NtscFieldView &NtscFrame::get_field(int parity) {
    return m_fields[parity];
}

std::shared_ptr<VbiData> NtscFrame::getVbiData() const {
    return m_vbi_data;
}

void NtscFrame::processVbi() {
    m_vbi_data = nullptr;

    // The Philips VBI codes appear on lines 16/17/18 of each field, and this
    // frame's buffer holds the two fields back to back: field 1 from line 1 and
    // field 2 from line 264, so field 2's code lines are 279/280/281.
    //
    // That layout holds only because NtscFrameReader identifies the field from
    // the phase of the vertical sync pattern and always starts a frame on field
    // 1; a frame started on field 2 would put the second field's codes one line
    // earlier.  Rather than depend on that, we probe every candidate line and
    // identify each code by its content — the IEC 60857 §10.1 codes all have
    // distinct nibble patterns, and nothing downstream needs to know which field
    // a code came from.  Probing 278 as well covers a frame that starts on the
    // wrong field, so a lock that slips cannot turn a CLV disc into a CAV one by
    // losing the 87FFFF marker.
    std::vector<int> codes;
    for (int line : {16, 17, 18, 278, 279, 280, 281}) {
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
    int is_clv = count(0x87ffff) >= 1;
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


    std::optional<int> chapter = std::nullopt;
    if (chapter_data != -1) {
        chapter = (chapter_data & 0xf00fff) == 0x800ddd ?
        std::make_optional(((chapter_data & 0xf0000) >> 16) * 10 + ((chapter_data & 0xf000) >> 12)) : std::nullopt;
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

    m_vbi_data = std::make_shared<VbiData>(is_lead_in, is_lead_out, is_clv, is_stop_code, chapter,
        clv_time_seconds, clv_picture_number, cav_picture_number, cx_enabled);

    if (m_log.isEnabled(eDebug, eDecoder)) {
        auto strings = m_vbi_data->asStrings();
        std::string code_list;
        for (int code : codes)
            code_list += std::format("{:06x} ", code & 0xffffff);
        m_log.debug(eDecoder, std::format("VBI {} {}|{}", code_list, strings[0], strings[1]));
    }
}

// Notice line starts at 1
int NtscFrame::processVbiLine(int line) {
    // The code starts at ~0.172 H (spec: 0.172 H or 0.188 H).  Sample 0 is at
    // 0H within a sample or two (measured from the colour burst, which starts
    // ~0.083 H; the sync tip itself is clamped out of this buffer), so the code's
    // rising edge really does land near 0.166 H here — a hair earlier on some
    // discs.  We begin the search at 0.15 H, in clean back porch: the burst ends
    // ~0.123 H and the first code bit rises ~0.166 H, so 0.15 H sits between them
    // (~24 samples clear of the burst).  0.165 H was too late — it coincided with
    // the code's own rising edge and failed the gate (alien1 lines 16/17 — the
    // programme status and CLV marker).
    int16_t *vbi = m_data->data<int16_t>() +
        (line - 1) * NtscInputBlock::c_samples_per_video_line +
            (int)(0.15 * NtscInputBlock::c_samples_per_video_line);

    if (HalfFloatUtil::half_to_float(vbi[0]) > 0.5) {
        m_log.debug(eDecoder, std::format("VBI line {}: signal present before code start", line));
        return -1;
    }

    for (int i = 0; i < (int)(0.005e-3 * NtscInputBlock::c_video_sampling_frequency); i++) {
        if (HalfFloatUtil::half_to_float(*vbi) > 0.7)
            goto found_start;
        vbi++;
    }
    //printf("VBI error: can not find start\n");
    return -1;

    found_start:

    int samples_per_half_bit = 1e-6 * NtscInputBlock::c_video_sampling_frequency;
    long half_bits = 0b01; // first two half bits are 01, already found
    int prev = 1;
    for (int i = 2; i < 48; i++) {
        // measure time to next transition
        int t = 0;
        while (t < samples_per_half_bit * 9 / 4) {
            float v = HalfFloatUtil::half_to_float(*vbi++);
            if (prev && v < 0.5 || !prev && v > 0.5) {
                prev = 1 - prev;
                break;
            }
            t++;
        }

        if (t < samples_per_half_bit * 3 / 4) {
            m_log.debug(eDecoder, std::format("VBI line {}: transition time too short", line));
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
