// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <utility>
#include <algorithm>
#include <cmath>
#include "filter/FFT.h"
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
    float line_1_high_sum = 0;
    float line_2_low_sum = 0;
    for (int i = 19; i < 259; i++) {
        line_1_high_sum += data[0 * MUSE_TOTAL_WIDTH + i];
        line_2_low_sum += data[1 * MUSE_TOTAL_WIDTH + i];
    }
    float blanking_sum =  0;
    for (int i = 127; i < 383; i++)
        blanking_sum += data[562 * MUSE_TOTAL_WIDTH + i] + data[1124 * MUSE_TOTAL_WIDTH + i];

    float avg_high = (float)line_1_high_sum / 240.0f;
    float avg_low = (float)line_2_low_sum / 240.0f;
    float avg_blanking = (float)blanking_sum / 512.0f;
    vector<pair<float, float>> v = {{16.0f , avg_low}, {128.0f, avg_blanking }, {239.0f, avg_high }};
    auto rescale = LinearRegression::linearRegression(v);
    return rescale;
}

namespace {

// Approximate median via nth_element; exact enough for noise estimation.
// Note: partially reorders v — pass a scratch copy if the order matters.
float median(vector<float> &v) {
    auto mid = v.begin() + v.size() / 2;
    nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Appends the residuals of a robust line fit over the window to out.  Removing
// level and tilt keeps low-frequency wander (DPLL residuals, hum) out of the
// noise estimate, and the median-based fit keeps a dropout spike in the window
// from dragging the fit and inflating every residual (a least-squares fit does).
void appendDetrendedResiduals(float const *samples, int count, vector<float> &out, float *center_out = nullptr) {
    int half = count / 2;
    vector<float> tmp(half);
    for (int i = 0; i < half; i++)
        tmp[i] = (samples[i + half] - samples[i]) / (float)half;
    float slope = median(tmp);
    size_t base = out.size();
    for (int i = 0; i < count; i++)
        out.push_back(samples[i] - slope * (i - (count - 1) / 2.0f));
    tmp.assign(out.begin() + base, out.end());
    float center = median(tmp); // window level at the window centre
    if (center_out != nullptr)
        *center_out = center;
    for (size_t i = base; i < out.size(); i++)
        out[i] -= center;
}

// Median absolute deviation scaled to equal σ for Gaussian noise; unlike an
// RMS it is not thrown off by a dropout spike in the window.
float robustSigma(vector<float> &residuals) {
    for (float &r : residuals)
        r = abs(r);
    return median(residuals) * 1.4826f;
}

}

FrameBuffer::NoiseEstimate FrameBuffer::EstimateNoise(float const *data) {
    // The reference regions are flat by construction, so after detrending, any
    // remaining fluctuation is channel noise.  The windows match EstimateRescale,
    // except that the line 1/2 windows stop at the VITS region.
    NoiseEstimate est{};
    vector<float> residuals;
    residuals.reserve(512);
    float clamp_mean0, clamp_mean1;
    appendDetrendedResiduals(data + 562 * MUSE_TOTAL_WIDTH + 127, 256, residuals, &clamp_mean0);
    appendDetrendedResiduals(data + 1124 * MUSE_TOTAL_WIDTH + 127, 256, residuals, &clamp_mean1);
    est.sigma_clamp = robustSigma(residuals);
    est.clamp_mean = (clamp_mean0 + clamp_mean1) / 2.0f;

    residuals.clear();
    appendDetrendedResiduals(data + 0 * MUSE_TOTAL_WIDTH + 19, c_vits_first_sample - 19, residuals);
    est.sigma_high = robustSigma(residuals);
    residuals.clear();
    appendDetrendedResiduals(data + 1 * MUSE_TOTAL_WIDTH + 19, c_vits_first_sample - 19, residuals);
    est.sigma_low = robustSigma(residuals);
    return est;
}

int FrameBuffer::AccumulateNoisePsd(float const *data, double *psd) {
    for (int row : {562, 1124}) {
        vector<float> residuals;
        residuals.reserve(256);
        appendDetrendedResiduals(data + row * MUSE_TOTAL_WIDTH + 127, 256, residuals);
        valarray<complex<double>> x(256);
        double window_power = 0;
        for (int i = 0; i < 256; i++) {
            double w = 0.5 - 0.5 * cos(2 * M_PI * i / 256); // Hann
            window_power += w * w;
            x[i] = residuals[i] * w;
        }
        FFT<double>::fft(x);
        for (int i = 0; i < 256; i++)
            psd[i] += norm(x[i]) / window_power;
    }
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
