//
// Created by staffanu on 6/22/24.
//

#include "NtscFrame.h"
#include "NtscFieldView.h"
#include "NtscInputBlock.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/HalfFloatUtil.h"

NtscFrame::NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager)
: m_frame_no(frame_no),
        m_input_offset(-1),
        m_input_samples_per_sample(0),
        m_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostRead)),
        m_burst_phase_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_total_video_lines), 4 /* 2 * sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_y_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_c_data(std::make_unique<musevk::VulkanBuffer>(
                manager, musevk::Size(NtscInputBlock::c_samples_per_video_line, NtscInputBlock::c_total_video_lines), 2 /* sizeof(float16) */,
                vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostNone)),
        m_fields({NtscFieldView(log, frame_no, m_data, m_burst_phase_data, m_y_data, m_c_data, 0),
                  NtscFieldView(log, frame_no, m_data, m_burst_phase_data, m_y_data, m_c_data, 1) }) {
}

void NtscFrame::set_frame_no(int frame_no, long input_offset, double input_samples_per_sample) {
    m_frame_no = frame_no;
    m_input_offset = input_offset;
    m_input_samples_per_sample = input_samples_per_sample;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

long NtscFrame::getInputOffset() const {
    return m_input_offset;
}

double NtscFrame::getInputSamplesPerNtscSample() const {
    return m_input_samples_per_sample;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::data() {
    return m_data;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::burst_phase_data() {
    return m_burst_phase_data;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::y_data() {
    return m_y_data;
}

std::shared_ptr<musevk::VulkanBuffer> &NtscFrame::c_data() {
    return m_c_data;
}

NtscFieldView &NtscFrame::get_field(int parity) {
    return m_fields[parity];
}

std::shared_ptr<VbiData> NtscFrame::getVbiData() const {
    return m_vbi_data;
}

void NtscFrame::processVbi() {
    m_vbi_data = nullptr;

    int vbi16 = processVbiLine(16);
    int vbi17 = processVbiLine(17);
    int vbi18 = processVbiLine(18);
    int vbi279 = processVbiLine(279);
    int vbi280 = processVbiLine(280);
    int vbi281 = processVbiLine(281);

    int is_lead_in = (vbi17 == 0x88FFFF) + (vbi18 == 0x88FFFF) + (vbi280 == 0x88FFFF) + (vbi281 == 0x88FFFF) >= 3;
    int is_lead_out = (vbi17 == 0x80EEEE) + (vbi18 == 0x80EEEE) + (vbi280 == 0x80EEEE) + (vbi281 == 0x80EEEE) >= 3;

    int is_clv = false;
    bool is_stop_code = false;
    int clv_time_data = -1;
    int chapter_data = -1;
    int programme_status_data = -1;
    std::optional<int> cav_picture_number = std::nullopt;
    if (vbi17 == 0x87ffff) {
        is_clv = true;
        if (vbi280 == vbi281)
            clv_time_data = vbi280;
        chapter_data = vbi18;
        programme_status_data = vbi16;
    } else if (vbi280 == 0x87ffff) {
        is_clv = true;
        if (vbi17 == vbi18)
            clv_time_data = vbi17;
        chapter_data = vbi281;
        programme_status_data = vbi279;
    } else {
        // CAV
        if (vbi16 == 0x82CFFF && vbi17 == 0x82CFFF || vbi279 == 0x82CFFF && vbi280 == 0x82CFFF)
            is_stop_code = true;

        if (vbi17 == vbi18 && (vbi17 & 0xf00fff) == 0x800ddd)
            chapter_data = vbi17;
        else if (vbi280 == vbi281 && (vbi280 & 0xf00fff) == 0x800ddd)
            chapter_data = vbi280;

        int cav_picture_number_data = -1;
        if (vbi17 == vbi18 && (vbi17 & 0xf00000) == 0xf00000)
            cav_picture_number_data = vbi17;
        else if (vbi280 == vbi281 && (vbi280 & 0xf00000) == 0xf00000)
            cav_picture_number_data = vbi280;

        if (cav_picture_number_data != -1)
            cav_picture_number = ((clv_time_data & 0xf0000) >> 16) * 10000 + ((clv_time_data & 0xf000) >> 12) * 1000 +
                ((clv_time_data & 0xf00) >> 8) * 100 + ((clv_time_data & 0xf0) >> 4) * 10 + (clv_time_data & 0xf);

        if (vbi16 == vbi279)
            programme_status_data = vbi16;
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

        int clv_picture_number_data = -1;
        if ((vbi16 & 0xf0f000) == 0x80d000)
            clv_picture_number_data = vbi16;
        else if ((vbi279 & 0xf0f000) == 0x80d000)
            clv_picture_number_data = vbi279;

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
        if (x41 ^ x42 ^ x44 ^ x51 || x41 ^ x43 ^ x44 ^ x52 || x42 ^ x43 ^ x44 ^ x53) {
            printf("Parity error X4 X5!\n");
        }
    }

    m_vbi_data = std::make_shared<VbiData>(is_lead_in, is_lead_out, is_clv, is_stop_code, chapter,
        clv_time_seconds, clv_picture_number, cav_picture_number, cx_enabled);

    printf("%x %x %x %x %x %x  %s|%s\n",
        processVbiLine(16), processVbiLine(17), processVbiLine(18),
        processVbiLine(279), processVbiLine(280), processVbiLine(281),
        m_vbi_data != nullptr ? m_vbi_data -> asStrings()[0].c_str() : "",
        m_vbi_data != nullptr ? m_vbi_data -> asStrings()[1].c_str() : "");
}

// Notice line starts at 1
int NtscFrame::processVbiLine(int line) {
    // The start of VBI information is at 0.188 H or 0.172 H. We start searching a bit earlier.
    int16_t *vbi = m_data->data<int16_t>() +
        (line - 1) * NtscInputBlock::c_samples_per_video_line +
            (int)(0.165 * NtscInputBlock::c_samples_per_video_line);

    if (HalfFloatUtil::half_to_float(vbi[0]) > 0.3) {
        printf("VBI error: not zero before start\n");
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
            printf("VBI error: transition time too short\n");
            return -1;
        } else if (t < samples_per_half_bit * 5 / 4) {
            half_bits = (half_bits << 1) | prev;
        } else if (t < samples_per_half_bit * 7 / 4) {
            printf("VBI error: transition time not one or two half bits\n");
            return -1;
        } else if (t < samples_per_half_bit * 9 / 4) {
            half_bits = (half_bits << 2) | ((1 - prev) << 1) | prev;
            i++;
        } else {
            printf("VBI error: transition time too long\n");
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
                printf("VBI error: invalid two bit pair bit %d: %lx\n", i, half_bits);
                return -1;
        }
    }
    return codeword;
}
