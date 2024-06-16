//
// Created by staffanu on 4/9/23.
//

#include <numeric>
#include <utility>
#include "musevk/VulkanBuffer.h"
#include "FrameBuffer.h"
#include "FieldBufferView.h"
#include "MuseConstants.h"
#include "musevk/HalfFloatUtil.h"

using namespace std;

FrameBuffer::FrameBuffer(Logger &log, int frame_no, std::shared_ptr<musevk::VulkanBuffer> data)
: m_frame_no(frame_no),
  m_input_offset(-1),
  m_input_samples_per_sample(0),
  m_data(std::move(data)),
  m_fields({FieldBufferView(log, frame_no, m_data, 0), FieldBufferView(log, frame_no, m_data, 1) }),
  m_disc_code(nullopt) {
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

std::pair<float, float> FrameBuffer::EstimateEq(float const *data) {
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
    vector<pair<float, float>> v = {{0.0f , avg_low}, {128.0f, avg_blanking }, {255.0f, avg_high }};
    // This is according to the documentation available, but it seems in reality the levels are 0 and 255?
    // vector<pair<float, float>> v = {{16.0f , avg_low}, {128.0f, avg_blanking }, {239.0f, avg_high }};
    auto eq = FrameBuffer::LinearRegression(v);
    return eq;
}

pair<float, float> FrameBuffer::LinearRegression(vector<pair<float, float>> const &values) {
    float sum_x = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first; });
    float sum_y = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.second; });
    float sum_xx = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first * v.first; });
    float sum_xy = accumulate(values.begin(), values.end(), 0.0f,
                              [](float acc, pair<float, float> v) { return acc + v.first * v.second; });
    int n = (int)values.size();
    float a = ((float)n * sum_xy - sum_x * sum_y) / ((float)n * sum_xx - sum_x * sum_x);
    float b = (sum_y - a * sum_x) / (float)n;
    return {a, b};
}

FieldBufferView &FrameBuffer::get_field(int parity) {
    return m_fields[parity];
}

std::optional<FrameBuffer::DiscCode> FrameBuffer::getDiscCode() const {
    return m_disc_code;
}

// Notice we use a pointer to the buffer from which the frame data is created before the frame data itself is ready
void FrameBuffer::ProcessControlData(float const *frame_data, std::pair<float, float> const &eq) {
    m_fields[0].ProcessControlData(frame_data + 558 * MUSE_TOTAL_WIDTH + 12, eq);
    m_fields[1].ProcessControlData(frame_data + 1120 * MUSE_TOTAL_WIDTH + 12, eq);
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
            m_disc_code = optional<DiscCode>({mode, cadr, fadr1, fadr2});
        } else {
            m_disc_code = nullopt;
            //cout << "CRC error" << endl;
        }
    } else {
        m_disc_code = nullopt;
        // cout << "cannot extract!" << endl;
    }
}
