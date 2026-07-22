// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <utility>
#include <algorithm>
#include <cmath>
#include "filter/FFT.h"
#include "util/RobustNoise.h"
#include "musevk/VulkanBuffer.h"
#include "FrameBuffer.h"
#include "FieldBufferView.h"
#include "MuseConstants.h"
#include "musevk/HalfFloatUtil.h"
#include "util/LinearRegression.h"

using namespace std;

FrameBuffer::FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data)
: m_frame_no(frame_no),
  m_input_offset(-1),
  m_input_samples_per_sample(0),
  m_data(std::move(data)),
  m_fields({FieldBufferView(log, frame_no, m_data, 0), FieldBufferView(log, frame_no, m_data, 1) }),
  m_disc_code(nullptr) {
}

void FrameBuffer::set_frame_no(int frame_no, long input_offset, double input_samples_per_sample) {
    m_frame_no = frame_no;
    m_input_offset = input_offset;
    m_input_samples_per_sample = input_samples_per_sample;
    m_fields[0].set_frame_no(frame_no);
    m_fields[1].set_frame_no(frame_no);
}

long FrameBuffer::getInputOffset() const {
    return m_input_offset;
}

double FrameBuffer::getInputSamplesPerMuseSample() const {
    return m_input_samples_per_sample;
}

std::shared_ptr<musevk::VulkanBuffer> &FrameBuffer::data() {
    return m_data;
}

std::pair<float, float> FrameBuffer::EstimateRescale(float const *data) {
    // Robust window centres rather than means: a dropout hitting a reference
    // window drags a mean (and with it the whole level mapping) but leaves the
    // median-based centre alone.
    float med_high = RobustNoise::robustCenter(data + 0 * MUSE_TOTAL_WIDTH + 19, 240);
    float med_low = RobustNoise::robustCenter(data + 1 * MUSE_TOTAL_WIDTH + 19, 240);
    float med_blanking = (RobustNoise::robustCenter(data + 562 * MUSE_TOTAL_WIDTH + 127, 256) +
                          RobustNoise::robustCenter(data + 1124 * MUSE_TOTAL_WIDTH + 127, 256)) / 2.0f;
    vector<pair<float, float>> v = {{16.0f , med_low}, {128.0f, med_blanking }, {239.0f, med_high }};
    return LinearRegression::linearRegression(v);
}

FrameBuffer::NoiseEstimate FrameBuffer::EstimateNoise(float const *data) {
    // The reference regions are flat by construction, so after detrending, any
    // remaining fluctuation is channel noise.  The windows match EstimateRescale,
    // except that the line 1/2 windows stop at the VITS region.
    NoiseEstimate est{};
    vector<float> residuals;
    residuals.reserve(512);
    float clamp_mean0, clamp_mean1;
    RobustNoise::appendDetrendedResiduals(data + 562 * MUSE_TOTAL_WIDTH + 127, 256, residuals, &clamp_mean0);
    RobustNoise::appendDetrendedResiduals(data + 1124 * MUSE_TOTAL_WIDTH + 127, 256, residuals, &clamp_mean1);
    est.sigma_clamp = RobustNoise::robustSigma(residuals);
    est.clamp_mean = (clamp_mean0 + clamp_mean1) / 2.0f;

    residuals.clear();
    RobustNoise::appendDetrendedResiduals(data + 0 * MUSE_TOTAL_WIDTH + 19, c_vits_first_sample - 19, residuals);
    est.sigma_high = RobustNoise::robustSigma(residuals);
    residuals.clear();
    RobustNoise::appendDetrendedResiduals(data + 1 * MUSE_TOTAL_WIDTH + 19, c_vits_first_sample - 19, residuals);
    est.sigma_low = RobustNoise::robustSigma(residuals);
    return est;
}

int FrameBuffer::AccumulateNoisePsd(float const *data, double *psd) {
    for (int row : {562, 1124})
        RobustNoise::accumulateDetrendedWindowPsd(data + row * MUSE_TOTAL_WIDTH + 127, 256, psd);
    return 2;
}

void FrameBuffer::ExtractVits(float const *data, float *out_line0, float *out_line1) {
    float const *row0 = data + 0 * MUSE_TOTAL_WIDTH + c_vits_first_sample;
    float const *row1 = data + 1 * MUSE_TOTAL_WIDTH + c_vits_first_sample;
    for (int i = 0; i < c_vits_sample_count; i++) {
        out_line0[i] = row0[i];
        out_line1[i] = row1[i];
    }
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return m_fields[parity];
}

std::shared_ptr<DiscCode> FrameBuffer::getDiscCode() const {
    return m_disc_code;
}

// Notice we use a pointer to the buffer from which the frame data is created before the frame data itself is ready
void FrameBuffer::ProcessControlData(float const *frame_data, std::pair<float, float> const &rescale) {
    m_fields[0].ProcessControlData(frame_data + 558 * MUSE_TOTAL_WIDTH + 12, rescale);
    m_fields[1].ProcessControlData(frame_data + 1120 * MUSE_TOTAL_WIDTH + 12, rescale);
}

void FrameBuffer::processDiscCode() {
    // Information on the Disc Code can be found in European Patent Application
    // EP0532277A2 "Method of recording information on video disk."

    // read all 76 bits
    int bits[76];
    for (int bit_ix = 0; bit_ix < 76; bit_ix++) {
        float d1 = HalfFloatUtil::half_to_float(m_data->data<uint16_t>()[563 * MUSE_TOTAL_WIDTH + 18 + 6 * bit_ix + 1]);
        float d2 = HalfFloatUtil::half_to_float(m_data->data<uint16_t>()[563 * MUSE_TOTAL_WIDTH + 18 + 6 * bit_ix + 4]);
        bits[bit_ix] = d2 > d1;
    }

    // makes an integer of the given bits (msb first)
    auto extractBits = [bits](int start, int length) -> int {
        int s = 0;
        for (int i = 0; i < length; i++)
            s += bits[start + i] ? (1 << (length - i - 1)) : 0;
        return s;
    };

    // checks if the crc8 of the first 68 bits (excluding ccrc) is equal to the given value
    auto checkCrc8 = [bits](int ccrc) -> bool {
        uint8_t out = 0x3a;
        for (int i = 0; i < 76; i++)
            out = ((out << 1) | (i < 68 ? bits[i] : 0)) ^ ((out & 0x80) ? 0xb3 : 0);
        return out == ccrc;
    };

    if (extractBits(0, 4 * 3) == 0b111111100100) {
        if (checkCrc8(extractBits(4 * 17, 4 * 2))) {
            int mode = extractBits(4 * 3, 4 * 2); // 8 bits
            int cadr = extractBits(4 * 5, 4 * 2); // 8 bits
            int fadr1 = extractBits(4 * 7, 4 * 5); // 20 bits
            int fadr2 = extractBits(4 * 12, 4 * 5); // 20 bits
            m_disc_code = std::make_shared<DiscCode>(mode, cadr, fadr1, fadr2);
        } else {
            m_disc_code = nullptr;
            //cout << "CRC error" << endl;
        }
    } else {
        m_disc_code = nullptr;
        // cout << "cannot extract!" << endl;
    }
}
