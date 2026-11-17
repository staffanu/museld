// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <ostream>
#include <bitset>
#include <format>
#include "AudioDecoder.h"
#include "logging/Logger.h"
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
  m_deinterleave_fill_bits(0),
  m_q(0),
  m_consecutive_failed_syncs(100), // start by searching for sync pattern
  m_symbol_locations(defaultSymbolLocations()),
  m_queue(),
  m_control_signals(),
  m_active_control_signal(0),
  m_active_audio_mode(MODE_UNKNOWN),
  m_bch_decoder(82, 137, true), // prim poly = x^7+x^3+1; all the specs say MUSE uses a SEC-DED code, but it is in fact DEC-TED
  m_range_bch_decoder(7, 11, false), // prim poly = x^3+x+1
  aModeChannel1Decoder(1),
  aModeChannel2Decoder(2),
  aModeChannel3Decoder(3),
  aModeChannel4Decoder(4),
  bModeChannel1Decoder(1),
  bModeChannel2Decoder(2),
  m_concealers{ErasureConcealer::create(ErasureConcealer::AutoregressiveModel, log, std::nullopt),
               ErasureConcealer::create(ErasureConcealer::AutoregressiveModel, log, std::nullopt)},
  m_samples_decoded(0),
  m_samples_erased(0),
  m_blocks_without_sync(0),
  m_rows_uncorrectable(0),
  m_range_words_uncorrectable(0),
  m_total_time_us(0) {
}

void AudioDecoder::reset() {
    std::fill(std::begin(m_deinterleave_data), std::end(m_deinterleave_data), false);
    m_deinterleave_buffer_start = 0;
    m_deinterleave_fill_bits = 0;
    m_q = 0;
    m_queue.clear();
    m_consecutive_failed_syncs = 100;
}

void AudioDecoder::decodeFrame(int frame_no,
                               shared_ptr<musevk::VulkanBuffer> const &audio_converted_freq,
                               AudioMode *audio_mode,
                               int *sample_count_out,
                               AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES]) {
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
    if (frame_no % 30 == 0) {
        if (m_samples_erased > 0)
            m_log.info(eAudio, std::format("MUSE audio: {} of {} samples concealed in the last second "
                                           "({} blocks without sync, {} rows and {} range words uncorrectable)",
                                           m_samples_erased, m_samples_decoded, m_blocks_without_sync,
                                           m_rows_uncorrectable, m_range_words_uncorrectable));
        m_samples_decoded = m_samples_erased = m_blocks_without_sync = 0;
        m_rows_uncorrectable = m_range_words_uncorrectable = 0;
    }

    *audio_mode = m_active_audio_mode;
    // The frame's samples with their erasure flags, per channel pair, until
    // the concealers have seen them
    std::vector<TwoChannelSampleWithErasureFlags> pairs[2];
    int no_not_updated_symbol_locations = 0;

    auto *ptr = audio_converted_freq->data<uint16_t>();
    unsigned width = audio_converted_freq->size().x_size;
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
                    float new_x = (prev_location.first * 255 + xin) / 256;
                    float new_y = (prev_location.second * 255 + yin) / 256;
                    m_symbol_locations[closest.first] = {new_x, new_y};
                } else {
                    no_not_updated_symbol_locations++;
                    if (no_not_updated_symbol_locations <= 3)
                        m_log.debug(eAudio,
                                    std::format("Not updating symbol location due to too far from default location: "
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
                m_q = (m_q + 1) % c_deinterleave_stages;
                // Nothing comes out until the deinterleaver has filled.  The
                // count stops there: it once ran on, and as an int it wrapped
                // after 2^31 bits, 26.5 minutes of playing, which closed this
                // gate and with it the audio for as long again
                if (m_deinterleave_fill_bits < c_deinterleave_stages * 1350)
                    m_deinterleave_fill_bits++;
                else
                    m_queue.push_back(out);
            }
        }
    }
    if (no_not_updated_symbol_locations > 3)
        m_log.debug(eAudio, std::format("Total number of not updated symbol location = {}",
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
                // Align the queue to the pattern and leave it there: the locked
                // path below consumes it with the frame, on this pass if the
                // whole frame is in, else on the next.  Consuming it here left
                // the control word at the head when the frame was not in yet,
                // which the next pass took for a missed sync, decoding two
                // frames 16 bits out of place before searching again
                m_consecutive_failed_syncs = 0;
                m_queue.erase(m_queue.cbegin(), ix);
                continue;
            }
            m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 1350 - 16);
        } else {
            if (!equal(m_queue.cbegin(), m_queue.cbegin() + 16, c_sync_pattern.cbegin())) {
                m_consecutive_failed_syncs++;
                m_log.warn(eAudio, "Sync pattern not recognized!");
            } else {
                m_consecutive_failed_syncs = 0;
            }
            m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 16);
        }
        // A block whose sync pattern is missing is still taken from the
        // stream (the lock survives two of them), but its samples are
        // erasures: it was decoded as if good before, and after a seek the
        // bits are those of two places on the disc
        const bool block_in_sync = m_consecutive_failed_syncs == 0;
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
                m_log.info(eAudio, std::format("audio control signal now: {}", bitset<22>(m_active_control_signal).to_string()));
                // A control word the table does not know keeps the mode in
                // use: a bit error in the word (the vote is over sixteen of
                // them, but a burst of errors turns it) switched the mode
                // to unknown for a couple of blocks and dropped the frame's
                // samples decoded so far (the Top Gun capture at 35.6 s)
                auto mode = detectModeFromControlData(majority);
                if (mode == MODE_UNKNOWN && m_active_audio_mode != MODE_UNKNOWN)
                    m_log.info(eAudio, "audio control signal unknown; keeping the audio mode in use");
                else if (mode != m_active_audio_mode) {
                    m_active_audio_mode = mode;
                    *audio_mode = mode;
                    pairs[0].clear();
                    pairs[1].clear();
                    m_log.info(eAudio, std::format("audio mode now: {}", (int)m_active_audio_mode));
                }
            }

            int deinterleave_matrix[16][82];
            for (int i = 0; i < 82; i++)
                for (int j = 0; j < 16; j++)
                    deinterleave_matrix[j][i] = m_queue[i * 16 + j];
            m_queue.erase(m_queue.cbegin(), m_queue.cbegin() + 1350 - 16 - 22); // 16 * 82 = 1312 bits

            bool row_ok[16];
            for (int i = 0; i < 16; i++) {
                row_ok[i] = m_bch_decoder.decode(deinterleave_matrix[i]);
                if (!row_ok[i])
                    m_rows_uncorrectable++;
                row_ok[i] = row_ok[i] && block_in_sync;
            }
            if (!block_in_sync)
                m_blocks_without_sync++;
            // One sample of every channel of the mode, from the words at
            // the given columns, into the pair vectors
            auto emit = [&](auto &&sample_of) {
                TwoChannelSampleWithErasureFlags s[2]{};
                sample_of(s);
                pairs[0].push_back(s[0]);
                if (m_active_audio_mode == MODE_A)
                    pairs[1].push_back(s[1]);
            };

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
                m_range_words_uncorrectable += !range1ok + !range2ok + !range3ok + !range4ok;

                for (int half = 0; half < 2; half++) {
                    const int c1 = half == 0 ? 1 : 9, c2 = half == 0 ? 17 : 25, c3 = half == 0 ? 34 : 42, c4 = half == 0 ? 50 : 58;
                    for (int i = 0; i < 16; i++)
                        emit([&](TwoChannelSampleWithErasureFlags s[2]) {
                            s[0].samples[0] = aModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][c1], row_ok[i], &s[0].erased[0]);
                            s[0].samples[1] = aModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][c2], row_ok[i], &s[0].erased[1]);
                            s[1].samples[0] = aModeChannel3Decoder.decodeSample(range3, range3ok, &deinterleave_matrix[i][c3], row_ok[i], &s[1].erased[0]);
                            s[1].samples[1] = aModeChannel4Decoder.decodeSample(range4, range4ok, &deinterleave_matrix[i][c4], row_ok[i], &s[1].erased[1]);
                        });
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
                m_range_words_uncorrectable += !range1ok + !range2ok;

                for (int third = 0; third < 3; third++) {
                    const int c1 = 1 + 11 * third, c2 = 34 + 11 * third;
                    for (int i = 0; i < 16; i++)
                        emit([&](TwoChannelSampleWithErasureFlags s[2]) {
                            s[0].samples[0] = bModeChannel1Decoder.decodeSample(range1, range1ok, &deinterleave_matrix[i][c1], row_ok[i], &s[0].erased[0]);
                            s[0].samples[1] = bModeChannel2Decoder.decodeSample(range2, range2ok, &deinterleave_matrix[i][c2], row_ok[i], &s[0].erased[1]);
                        });
                }
            } else
                m_log.warn(eAudio, "Unknown audio mode");
        }
    }
    // Concealment, then the frame's output; the concealers keep the count
    int sample_count = 0;
    if (!pairs[0].empty()) {
        int erased = 0;
        for (const auto &s : pairs[0])
            erased += s.erased[0] + s.erased[1];
        for (const auto &s : pairs[1])
            erased += s.erased[0] + s.erased[1];
        if (erased > 0)
            m_log.debug(eAudio, std::format("frame {}: {} of {} audio samples erased", frame_no, erased,
                                            pairs[0].size() * 2 + pairs[1].size() * 2));
        m_samples_erased += erased;
        m_samples_decoded += (int)(pairs[0].size() * 2 + pairs[1].size() * 2);
        const auto front = m_concealers[0]->processSamples(pairs[0]);
        const auto rear = m_active_audio_mode == MODE_A ? m_concealers[1]->processSamples(pairs[1])
                                                         : std::vector<TwoChannelSample>{};
        sample_count = (int)std::min<size_t>(front.size(), MAX_AUDIO_OUTPUT_SAMPLES);
        for (int i = 0; i < sample_count; i++) {
            output_samples[i].samples[0] = front[i].samples[0];
            output_samples[i].samples[1] = front[i].samples[1];
            if (m_active_audio_mode == MODE_A && i < (int)rear.size()) {
                output_samples[i].samples[2] = rear[i].samples[0];
                output_samples[i].samples[3] = rear[i].samples[1];
            }
        }
    }
    *sample_count_out = sample_count;

    auto t1 = chrono::high_resolution_clock::now();
    m_total_time_us += chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
    if (frame_no % 30 == 0) {
        m_log.info(eAudio | ePerformance, std::format("Avg audio decoding time last second: {:.1f} ms/frame",
                                                      (double) m_total_time_us / 1000.0/ 30));
        m_total_time_us = 0;
    }
}
