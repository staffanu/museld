//
// Created by staffanu on 4/9/23.
//

#include <ostream>
#include <bitset>
#include <fmt/format.h>
#include "AudioDecoder.h"
#include "MuseBuffer.h"
#include "Logger.h"
#include "musevk/HalfFloatUtil.h"

using namespace std;

#define SQUARE(x) ((x) * (x))

const vector<pair<float, float>> AudioDecoder::c_default_symbol_locations = defaultSymbolLocations();

const array<array<bool, 3>, 8> AudioDecoder::c_symbol_bits = {
        false, false, false, // 0, 0
        false, false, true, // 0, 1
        false, true, true, // 0, 2
        true, false, false, // 1, 0
        false, true, false, // 1, 2
        true, false, true, // 2, 0
        true, true, true, // 2, 1
        true, true, false // 2, 2
};

const array<bool, 16> AudioDecoder::c_sync_pattern = {
        false, false, false, true, false, false, true, true,
        false, true, false, true, true, true, true, false
};

AudioMode AudioDecoder::detectModeFromControlData(uint32_t control_data) {
    // I found no documentation whatsoever for the control signal, so this is just a table of
    // what I have seen in examples.  There is probably more information than just the MODE A/B
    // designation.
    switch (control_data) {
        case 0b0110010000000010100001:
        case 0b0110011001000010101001:
        case 0b0110010000000010100011: // Cliffhanger
            return MODE_A;
        case 0b1001100000000010100011:
        case 0b1001100000000010100001:
            return MODE_B;
        default:
            return MODE_UNKNOWN;
    }
}

vector<pair<float, float>> AudioDecoder::defaultSymbolLocations() {
    vector<pair<float, float>> symbol_locations;
    for (int i = 0; i <= 2; i++)
        for (int j = 0; j <= 2; j++)
            if (i != 1 || j != 1)
                symbol_locations.emplace_back(pair(28 + i * 100, 28 + j * 100));
    return symbol_locations;
}

AudioDecoder::AudioDecoder(Logger &log)
: m_log(log),
  m_deinterleave_data{},
  m_deinterleave_buffer_start(0),
  m_total_deinterleaved_bits(0),
  m_q(0),
  m_consecutive_failed_syncs(100), // start by searching for sync pattern
  m_symbol_locations(defaultSymbolLocations()),
  m_queue(),
  m_control_signals(),
  m_active_control_signal(0),
  m_active_audio_mode(MODE_UNKNOWN),
  m_bch_decoder(82, 74, 137),
  m_range_bch_decoder(7, 3, 11),
  aModeChannel1Decoder(1),
  aModeChannel2Decoder(2),
  aModeChannel3Decoder(3),
  aModeChannel4Decoder(4),
  bModeChannel1Decoder(1),
  bModeChannel2Decoder(2),
  m_total_time_us(0) {
}

void AudioDecoder::decodeFrame(int frame_no,
                               MuseBuffer &audio_converted_freq,
                               AudioMode &audio_mode,
                               size_t &sample_count,
                               AudioFrame output_samples[c_max_output_samples]) {
    auto t0 = chrono::high_resolution_clock::now();

    if (frame_no % 30 == 0 && m_log.isEnabled(eDebug, eAudio)) {
        ostringstream ss;
        ss << "Symbol locations: ";
        for (int i = 0; i < 8; i++)
            ss << "(" << (int)m_symbol_locations[i].first << ", " << (int)m_symbol_locations[i].second << ") ";
        m_log.debug(eAudio, ss.str());

        m_bch_decoder.printStatistics(m_log, "Audio BchDecoder");
        m_bch_decoder.resetStatistics();
        m_range_bch_decoder.printStatistics(m_log, "Range bits BchDecoder");
        m_range_bch_decoder.resetStatistics();
    }

    audio_mode = m_active_audio_mode;
    sample_count = 0;
    int no_not_updated_symbol_locations = 0;

    auto *ptr = audio_converted_freq.getVulkanBuffer()->data<ushort>();
    unsigned width = audio_converted_freq.width();
    for (int row = 0; row < 88; row++) {
        int start_col = 9 + (row >= 40 && row < 44 || row >= 84 ? 78 : 0);
        int end_col = 9 + 348;
        for (int col = start_col; col < end_col; col += 2) {
            float xin = HalfFloatUtil::half_to_float(ptr[row * width + col]);
            float yin = HalfFloatUtil::half_to_float(ptr[row * width + col + 1]);

            pair<int, float> closest = {-1, 1e10f};
            pair<int, float> second_closest = {-1, 1e10f};
            for (int i = 0; i < 8; i++) {
                float dist = abs(xin - m_symbol_locations[i].first) + abs(yin - m_symbol_locations[i].second);
                if (dist < closest.second) {
                    second_closest = closest;
                    closest = {i, dist};
                } else if (dist < second_closest.second)
                    second_closest = {i, dist};
            }

            if (abs(closest.second - second_closest.second) > 40) {
                if (SQUARE(xin - c_default_symbol_locations[closest.first].first) +
                        SQUARE(yin - c_default_symbol_locations[closest.first].second) < 49 * 49 + 49 * 49) {
                    pair<float, float> prev_location = m_symbol_locations[closest.first];
                    float new_x = (prev_location.first * 15 + xin) / 16;
                    float new_y = (prev_location.second * 15 + yin) / 16;
                    m_symbol_locations[closest.first] = {new_x, new_y};
                } else {
                    no_not_updated_symbol_locations++;
                    if (no_not_updated_symbol_locations <= 3)
                        m_log.debug(eAudio,
                                    fmt::format("Not updating symbol location due to too far from default location: "
                                                "presumed symbol {}=({}, {})", closest.first, xin, yin));
                }
            }

            array<bool, 3> bits = c_symbol_bits[closest.first];
            for (int i = 0;  i < 3; i++) {
                bool b = bits[i];
                if (m_deinterleave_buffer_start-- == 0)
                    m_deinterleave_buffer_start = c_deinterleave_buffer_size - 1;
                m_deinterleave_data[m_deinterleave_buffer_start] = b;

                bool out = m_deinterleave_data[(m_q * 1350 + m_deinterleave_buffer_start) % c_deinterleave_buffer_size];
                m_total_deinterleaved_bits += 1;
                m_q = (m_q + 1) % c_deinterleave_stages;
                if (m_total_deinterleaved_bits > c_deinterleave_stages * 1350) {
                    m_queue.push_back(out);
                }
            }
        }
    }
    if (no_not_updated_symbol_locations > 3)
        m_log.debug(eAudio, fmt::format("Total number of not updated symbol location = {}",
                                        no_not_updated_symbol_locations));

    bool printed_searching = false;
    while (m_queue.size() >= 1350) {
        if (m_consecutive_failed_syncs > 2) {
            if (!printed_searching) {
                m_log.info(eAudio, "searching for sync pattern");
                printed_searching = true;
            }
            auto ix = search(m_queue.cbegin(), m_queue.cend(), c_sync_pattern.cbegin(), c_sync_pattern.cend());
            if (ix != m_queue.cend()) {
                m_consecutive_failed_syncs = 0;
                m_queue.erase(m_queue.cbegin(), ix + 16);
            } else {
                m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 1350 - 16);
            }
        } else {
            if (!equal(m_queue.cbegin(), m_queue.cbegin() + 16, c_sync_pattern.cbegin())) {
                m_consecutive_failed_syncs++;
                m_log.warn(eAudio, "Sync pattern not recognized!");
            } else {
                m_consecutive_failed_syncs = 0;
            }
            m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 16);
        }
        if (m_consecutive_failed_syncs <= 2 && m_queue.size() >= 1350 - 16) {
            uint32_t control_signal = 0;
            for (int i = 0; i < 22; i++) {
                control_signal |= m_queue.front() ? 1 << (21 - i) : 0;
                m_queue.pop_front();
            }
            m_control_signals.push_front(control_signal);
            if (m_control_signals.size() > 16)
                m_control_signals.pop_back();

            map<uint32_t , int> control_freqs;
            for (const uint32_t c : m_control_signals)
                control_freqs[c]++;
            uint32_t majority = max_element(control_freqs.cbegin(), control_freqs.cend(),
                                  [](pair<uint32_t , int> const &left, pair<uint32_t, int> const &right) -> bool {
                return left.second < right.second;
            })->first;
            if (majority != m_active_control_signal) {
                m_active_control_signal = majority;
                m_log.info(eAudio, fmt::format("audio control signal now: {}", bitset<22>(m_active_control_signal).to_string()));
                auto mode = detectModeFromControlData(majority);
                if (mode != m_active_audio_mode) {
                    m_active_audio_mode = mode;
                    audio_mode = mode;
                    sample_count = 0;
                    m_log.info(eAudio, fmt::format("audio mode now: {}", (int)m_active_audio_mode));
                }
            }

            int deinterleave_matrix[16][82];
            for (int i = 0; i < 82; i++)
                for (int j = 0; j < 16; j++)
                    deinterleave_matrix[j][i] = m_queue[i * 16 + j];
            m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 1350 - 16 - 22); // 16 * 82 = 1312 bits

            bool row_ok[16];
            for (int i = 0; i < 16; i++)
                row_ok[i] = m_bch_decoder.decode(deinterleave_matrix[i]);

            if (m_active_audio_mode == MODE_A) {
                int range1[7]; bool range1ok;
                int range2[7]; bool range2ok;
                int range3[7]; bool range3ok;
                int range4[7]; bool range4ok;
                for (int i = 0; i < 7; i++) {
                    range1[i] = deinterleave_matrix[i][0];
                    range2[i] = deinterleave_matrix[i + 8][0];
                    range3[i] = deinterleave_matrix[i][33];
                    range4[i] = deinterleave_matrix[i + 8][33];
                }
                range1ok = m_range_bch_decoder.decode(range1);
                range2ok = m_range_bch_decoder.decode(range2);
                range3ok = m_range_bch_decoder.decode(range3);
                range4ok = m_range_bch_decoder.decode(range4);

                for (int i = 0; i < 16; i++) {
                    output_samples[sample_count].samples[0] = aModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][1], row_ok[i]);
                    output_samples[sample_count].samples[1] = aModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][17], row_ok[i]);
                    output_samples[sample_count].samples[2] = aModeChannel3Decoder.decodeSample(range3, range3ok, &deinterleave_matrix[i][34], row_ok[i]);
                    output_samples[sample_count].samples[3] = aModeChannel4Decoder.decodeSample(range4, range4ok, &deinterleave_matrix[i][50], row_ok[i]);
                    sample_count++;
                }
                for (int i = 0; i < 16; i++) {
                    output_samples[sample_count].samples[0] = aModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][9], row_ok[i]);
                    output_samples[sample_count].samples[1] = aModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][25], row_ok[i]);
                    output_samples[sample_count].samples[2] = aModeChannel3Decoder.decodeSample(range3, range3ok, &deinterleave_matrix[i][42], row_ok[i]);
                    output_samples[sample_count].samples[3] = aModeChannel4Decoder.decodeSample(range4, range4ok, &deinterleave_matrix[i][58], row_ok[i]);
                    sample_count++;
                }
            } else if (m_active_audio_mode == MODE_B) {
                int range1[7]; bool range1ok;
                int range2[7]; bool range2ok;
                for (int i = 0; i < 7; i++) {
                    range1[i] = deinterleave_matrix[i][0];
                    range2[i] = deinterleave_matrix[i + 8][0];
                }
                range1ok = m_range_bch_decoder.decode(range1);
                range2ok = m_range_bch_decoder.decode(range2);

                for (int i = 0; i < 16; i++) {
                    output_samples[sample_count].samples[0] = bModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][1], row_ok[i]);
                    output_samples[sample_count].samples[1] = bModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][34], row_ok[i]);
                    sample_count++;
                }
                for (int i = 0; i < 16; i++) {
                    output_samples[sample_count].samples[0] = bModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][12], row_ok[i]);
                    output_samples[sample_count].samples[1] = bModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][45], row_ok[i]);
                    sample_count++;
                }
                for (int i = 0; i < 16; i++) {
                    output_samples[sample_count].samples[0] = bModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][23], row_ok[i]);
                    output_samples[sample_count].samples[1] = bModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][56], row_ok[i]);
                    sample_count++;
                }
            } else
                m_log.warn(eAudio, "Unknown audio mode");
        }
    }

    auto t1 = chrono::high_resolution_clock::now();
    m_total_time_us += chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    if (frame_no % 30 == 0) {
        m_log.info(eAudio | ePerformance, fmt::format("Avg audio decoding time last second: {:.1f} ms/frame",
                                                      (double) m_total_time_us / 1000.0/ 30));
        m_total_time_us = 0;
    }
}
