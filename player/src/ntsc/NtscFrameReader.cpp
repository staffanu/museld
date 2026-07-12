// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include <fcntl.h>
#include <unistd.h>
#include <format>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "NtscFrameReader.h"

#include "NtscRfDemodulator.h"
#include "logging/Logger.h"
#include "util/Interpolate.h"

#ifndef O_BINARY
#  define O_BINARY 0
#endif
#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif

using namespace std;

NtscFrameReader::NtscFrameReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        const std::string &filename, InputFormat input_format, double sample_rate,
        double initial_seek_seconds, bool benchmark_shaders, bool efm_enabled,
        const std::optional<std::string> &output_filename)
        : FrameReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_file_fd(-1),
          m_sample_rate(sample_rate / NtscRfDemodulatorConstants::c_video_decimation_rate),
          m_input_samples_decimation_rate(1),
          m_demodulator(nullptr),
          m_input_buffer(nullptr),
          m_input_dropout_buffer(nullptr),
          m_input_sub_buffer_input_offsets{},
          m_last_input_sub_buffer_ix_read(0),
          m_t(0.0),
          m_half_line_sync_pattern_error_sum(0.0),
          m_half_line_sync_pattern_eq_error_sum(0.0),
          m_half_line_sync_pattern_br_error_sum(0.0),
          m_vert_sync_half_line_pattern(0L),
          m_sample_ix(1),
          m_line(1),
          m_state(eSearching),
          m_lower_percentile_filter(NtscInputBlock::c_samples_per_video_line * NtscInputBlock::c_total_video_lines, 0.005f, 0.f),
          m_consecutive_good_syncs(0),
          m_missed_half_line_vert_sync_patterns(0),
          m_frame_start_offset(0L),
          m_sample_history{},
          m_sample_history_ix(0),
          m_error_sum(0) {

    m_demodulator = new NtscRfDemodulator(log, executable_dir, m_filename, sample_rate, vulkan_manager, input_format, benchmark_shaders, efm_enabled);
    // m_sample_rate = 31.25e6;

    m_bytes_per_sample = 4;
    m_output_multiplier = 1.0;
    m_output_add = 0.0;
}

bool NtscFrameReader::initialize(std::vector<std::unique_ptr<NtscInputBlock>> &buffers) {
    if (m_demodulator == nullptr) {
        m_file_fd = open(m_filename.c_str(), O_RDONLY | O_BINARY | O_NONBLOCK);
        if (m_file_fd == -1)
            throw runtime_error(std::format("NtscFrameReader: Unable to open input file {}", m_filename));
#ifdef linux
        if (filesystem::is_fifo(m_filename)) {
            m_log.debug(eInput, std::format("Pipe size: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
            fcntl(m_file_fd, F_SETPIPE_SZ, 1024 * 1024);
            m_log.debug(eInput, std::format("Pipe size now: {}", fcntl(m_file_fd, F_GETPIPE_SZ)));
        }
#endif
    } else {
        m_demodulator->initialize(NtscRfDemodulatorConstants::c_number_of_block_buffers);
    }

    m_input_samples_per_sample_ref = m_sample_rate / NtscInputBlock::c_video_sampling_frequency;
    m_input_samples_per_sample = m_input_samples_per_sample_ref;
    m_omega = 2 * M_PI * 3000 / m_sample_rate;
    m_zeta = 0.85;
    m_Ts = m_input_samples_per_sample_ref * NtscInputBlock::c_samples_per_video_line;
    m_G1 = 1 - exp(-2 * m_zeta * m_omega * m_Ts);
    m_G2 = 1 + exp(-2 * m_omega * m_zeta * m_Ts) -
           2 * exp(-m_omega * m_zeta * m_Ts) * cos(m_omega * m_Ts * sqrt(1 - m_zeta * m_zeta));
    m_GpdGvco = m_input_samples_per_sample_ref * NtscInputBlock::c_samples_per_video_line;
    m_g1 = m_G1 / m_GpdGvco;
    m_g2 = m_G2 / m_GpdGvco;

    m_log.debug(eInput, std::format("NtscFrameReader: m_g1={:.5f} m_g2={:.7f}", m_g1, m_g2));

    m_input_buffer = (uint8_t *)calloc(m_bytes_per_sample, c_input_buffer_size);
    m_input_dropout_buffer = (uint8_t *)calloc(1, c_input_buffer_size);
    assert(m_input_buffer != nullptr && m_input_dropout_buffer != nullptr);

    return FrameReader::initialize(buffers);
}

void NtscFrameReader::cleanup() {
    FrameReader::cleanup();

    free(m_input_buffer);
    free(m_input_dropout_buffer);

    if (m_demodulator != nullptr) {
        m_demodulator->cleanup();
        delete m_demodulator;
    }

    if (m_file_fd != -1)
        close(m_file_fd);
}

void NtscFrameReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        if (m_demodulator != nullptr) {
            m_demodulator->seek(seconds);
        } else {
            std::unique_lock<std::mutex> lock(m_mutex);

            off_t samples_to_seek = (off_t) (seconds * NtscInputBlock::c_video_sampling_frequency * m_input_samples_per_sample);
            off_t bytes_to_seek = m_bytes_per_sample * samples_to_seek;
            m_log.info(eInput, std::format("Seeking relative time {} s, {} samples, {} bytes.",
                                           seconds, samples_to_seek, bytes_to_seek));
            lseek(m_file_fd, bytes_to_seek, SEEK_CUR);

            // discard content in existing input buffers
            move(m_filled_input_buffers.begin(), m_filled_input_buffers.end(),
                 back_inserter(m_vacant_input_buffers));
            m_filled_input_buffers.clear();
            m_cv_vacant.notify_one();
        }
        setUnlocked(); // do not wait to discover that we lost sync
    }
}

void NtscFrameReader::setEfmEnabled(bool enabled) {
    if (m_demodulator != nullptr)
        m_demodulator->setEfmEnabled(enabled);
}

void NtscFrameReader::threadFunc() {
    unique_ptr<NtscInputBlock> output_block = nullptr;

    for (m_last_input_sub_buffer_ix_read = 0; m_last_input_sub_buffer_ix_read < c_number_of_input_sub_buffers; m_last_input_sub_buffer_ix_read++) {
        readInput(nullptr);
    }
    m_last_input_sub_buffer_ix_read = c_number_of_input_sub_buffers - 1;

    for (;;) {
        if (output_block == nullptr) {
            unique_lock<std::mutex> lock(m_mutex);
            if (m_input_is_realtime && m_vacant_input_buffers.empty()) {
                // discard a filled output_block -- this is better than having the writer to the fifo wait
                m_log.warn(eInput, "Discarding filled block due to overrun");
                assert(!m_filled_input_buffers.empty());
                m_vacant_input_buffers.push_back(std::move(m_filled_input_buffers.back()));
                m_filled_input_buffers.pop_back();
            }
            m_cv_vacant.wait(lock, [this]{return m_stop_request || !m_vacant_input_buffers.empty();});
            if (m_stop_request) {
                m_log.info(eInput, "NtscFrameReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data.clear();
        }

        if (!process(output_block)) {
            m_log.info(eInput, "NtscFrameReader: end of file");
            break;
        }

        output_block->input_offset = m_frame_start_offset;
        output_block->input_samples_per_video_sample = m_input_samples_per_sample * m_input_samples_decimation_rate;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(output_block));
        output_block = nullptr;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool NtscFrameReader::readInput(std::unique_ptr<NtscInputBlock> const &output_block) {
    uint8_t *read_ptr = m_input_buffer + m_bytes_per_sample * c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;
    uint8_t *dropout_read_ptr = m_input_dropout_buffer + c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;

    m_input_samples_decimation_rate = NtscRfDemodulatorConstants::c_video_decimation_rate;
    auto block = m_demodulator->getNextDemodulatedBlock();
    if (block == nullptr) {
        m_log.info(eInput, "NtscFrameReader: no more demodulated blocks");
        return false;
    }
    memcpy(read_ptr, block->video_data->data<float>(), NtscRfDemodulatorConstants::c_video_block_size * sizeof(float));
    memcpy(dropout_read_ptr, block->dropouts->data<uint8_t>(), NtscRfDemodulatorConstants::c_video_block_size * sizeof(uint8_t));
    m_input_sub_buffer_input_offsets[m_last_input_sub_buffer_ix_read] = block->input_offset;

    if (output_block != nullptr) {
        if (m_state == eLocked)
            output_block->efm_data.insert(output_block->efm_data.end(), block->efm_data.begin(), block->efm_data.end());
        else
            output_block->efm_data.clear();
    }

    m_demodulator->returnBlock(block);

    // int fd = open("videodata.floats",
    //               O_WRONLY | O_TRUNC | O_CREAT,
    //               S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    // write(fd, read_ptr, NtscRfDemodulatorConstants::c_video_block_size * sizeof(float));
    // close(fd);

    return true;
}

bool NtscFrameReader::resample(float *sample_out, uint8_t *dropout_out,
                               double input_samples_per_sample,
                               std::unique_ptr<NtscInputBlock> const &output_block) {

    m_t += input_samples_per_sample;

    const size_t read_pos = (size_t)m_t & c_input_buffer_size_mask;
    const auto i3 = (read_pos - 3) & c_input_buffer_size_mask;
    const auto i2 = (read_pos - 2) & c_input_buffer_size_mask;
    const auto i1 = (read_pos - 1) & c_input_buffer_size_mask;
    const auto i0 = read_pos;

    const float b0 = ((float *)m_input_buffer)[i3];
    const float b1 = ((float *)m_input_buffer)[i2];
    const float b2 = ((float *)m_input_buffer)[i1];
    const float b3 = ((float *)m_input_buffer)[i0];

    *sample_out = (float)(cubicInterpolate(b0, b1, b2, b3, (float)(m_t - (size_t)m_t)) * m_output_multiplier + m_output_add);
    *dropout_out = m_input_dropout_buffer[read_pos];

    auto current_sub_buffer_index = read_pos >> c_input_sub_buffer_size_bits;
    if ((m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers != current_sub_buffer_index) {
        m_last_input_sub_buffer_ix_read = (m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers;

        if (!readInput(output_block))
            return false;
    }
    return true;
}


bool NtscFrameReader::process(std::unique_ptr<NtscInputBlock> const &output_block) {
    auto *output = output_block->video_data->data<float>();
    auto *dropout_output = output_block->dropout_data->data<uint8_t>();

    m_frame_start_offset = m_input_sub_buffer_input_offsets[((size_t)m_t & c_input_buffer_size_mask) >> c_input_sub_buffer_size_bits];

    float sample;
    uint8_t dropout;
    while (resample(&sample, &dropout, m_input_samples_per_sample, output_block)) {
        m_lower_percentile_filter.update(sample);

        int output_index = NtscInputBlock::c_samples_per_video_line * (m_line - 1) + m_sample_ix - 1;
        output[output_index] = sample;
        dropout_output[output_index] = dropout;
        m_sample_history[m_sample_history_ix++] = sample;
        if (m_sample_history_ix == c_sample_history_size)
            m_sample_history_ix = 0;

        // notice sample_ix is 1 based
        int half_line_sample_ix = m_sample_ix <= NtscInputBlock::c_samples_per_video_line / 2 ?
            m_sample_ix : m_sample_ix - NtscInputBlock::c_samples_per_video_line / 2;
        bool right_half = m_sample_ix >= NtscInputBlock::c_samples_per_video_line / 2 + 1;
        if (half_line_sample_ix == 1) {
            m_half_line_sync_pattern_error_sum = 0;
            m_half_line_sync_pattern_eq_error_sum = 0;
            m_half_line_sync_pattern_br_error_sum = 0;
        }

        if (m_state == eLocked && (m_line <= 9 || m_line >= 264 && m_line <= 271 || m_line == 263 && right_half || m_line == 272 && !right_half)) {
            int expected_pulse_samples = expectedPulseSamplesForHalfLine(m_line, right_half);
            float expected_voltage = half_line_sample_ix <= expected_pulse_samples ? 0.f : 0.3f;
            m_half_line_sync_pattern_error_sum += pow(sample - expected_voltage, 2.f);

            if (half_line_sample_ix == NtscInputBlock::c_samples_per_video_line / 2) {
                if (m_half_line_sync_pattern_error_sum < 0.03 * NtscInputBlock::c_samples_per_video_line / 2) {
                    //printf("line %d %d, sum %f\n", m_line, right_half, m_half_line_sync_pattern_error_sum);
                    m_missed_half_line_vert_sync_patterns = 0;
                } else {
                    if (m_missed_half_line_vert_sync_patterns < 3)
                        m_missed_half_line_vert_sync_patterns += 1;
                    else {
                        m_log.warn(eInput, std::format("Missed line pulses: New state eSearching at line {}", m_line));
                        m_state = eSearching;
                    }
                }
            }
        } else if (m_state == eLockedHoriz) {
            float expected_voltage_eq = half_line_sample_ix <= c_equalizationPulseSamples ? 0.f : 0.3f;
            float expected_voltage_br = half_line_sample_ix <= c_broadPulseSamples ? 0.f : 0.3f;

            m_half_line_sync_pattern_eq_error_sum += pow(sample - expected_voltage_eq, 2.f);
            m_half_line_sync_pattern_br_error_sum += pow(sample - expected_voltage_br, 2.f);

            if (half_line_sample_ix == NtscInputBlock::c_samples_per_video_line / 2) {
                if (m_half_line_sync_pattern_eq_error_sum <= 0.02 * NtscInputBlock::c_samples_per_video_line / 2)
                    m_vert_sync_half_line_pattern = (m_vert_sync_half_line_pattern << 2) | 0b01;
                else if (m_half_line_sync_pattern_br_error_sum <= 0.02 * NtscInputBlock::c_samples_per_video_line / 2)
                    m_vert_sync_half_line_pattern = (m_vert_sync_half_line_pattern << 2) | 0b11;
                else
                    m_vert_sync_half_line_pattern = (m_vert_sync_half_line_pattern << 2) | 0b00;

                if ((m_vert_sync_half_line_pattern & 0xfffffffff) == 0b010101010101111111111111010101010101L) {
                    m_log.info(eInput, std::format("New state eLocked at line {}", m_line));
                    m_state = eLocked;
                    m_line = 9;
                    // TODO: in the MUSE version, we copy the first four lines to to make the frame complete -- any point doing this here?
                }
            }
        }

        if (m_sample_ix == 10) { // The sync defines the start of a line at sample 1, but we wait so we can read the value it settled at
            float sample0 = m_sample_history[(m_sample_history_ix + c_sample_history_size - 12) % c_sample_history_size];
            float sample1 = m_sample_history[(m_sample_history_ix + c_sample_history_size - 11) % c_sample_history_size];
            float sample2 = m_sample_history[(m_sample_history_ix + c_sample_history_size - 10) % c_sample_history_size];
            float sample3 = m_sample_history[(m_sample_history_ix + c_sample_history_size - 9) % c_sample_history_size];
            float sample_late = m_sample_history[(m_sample_history_ix + c_sample_history_size - 1) % c_sample_history_size];

            bool m_sync_is_good = sample0 > 0.15f && sample3 < 0.15f && sample_late < 0.15f;

            if (m_sync_is_good) {
                // positive means we sampled too late (sync is early)
                // negative means we sampled too early (sync is late)
                // We expect the sync between sample1 and sample2
                double new_error;
                if (sample1 < 0.15)
                    new_error = 1.0;
                else if (sample2 > 0.15)
                    new_error = -1.0;
                else {
                    // sync is between sample1 and sample2 -- try to determine where
                    float mid_level = 0.5f * (sample0 + sample_late); // determined by sample before sync and late during sync
                    float mid_s12 = 0.5f * (sample1 + sample2);
                    new_error = clamp(10.f * (mid_level - mid_s12), -1.f, 1.f);
                }
                m_error_sum = std::clamp(m_error_sum + new_error, -10.0, 10.0);
                m_input_samples_per_sample =
                        m_input_samples_per_sample_ref - new_error * m_g1 - m_error_sum * m_g2;
                // printf("%.2f %.2f %.2f %.2f  %.2f: %.2f\n", sample0, sample1, sample2, sample3, sample_late, new_error);
            }
            if (m_state == eSearching) {
                m_consecutive_good_syncs = m_sync_is_good ? m_consecutive_good_syncs + 1 :
                                           m_consecutive_good_syncs >= 2 ? m_consecutive_good_syncs - 2 : 0;
                if (m_consecutive_good_syncs >= 50) {
                    m_log.info(eInput, std::format("New state eLockedHoriz at line {}", m_line));
                    m_state = eLockedHoriz;
                }
            }
            if ((m_state == eSearching && ((m_consecutive_good_syncs < 5 && m_line == 50) || m_line == 100)) ||
                (m_state == eLockedHoriz && m_line == NtscInputBlock::c_total_video_lines)) {
                m_consecutive_good_syncs = 0;
                m_error_sum = 0;
                m_input_samples_per_sample = m_input_samples_per_sample_ref;

                if (m_state == eLockedHoriz) {
                    m_log.info(eInput, "Locked horizontally, but frame pulses not found");
                }
                m_state = eSearching;
                m_line = 3;
                m_sample_ix = 263; // "random", start search from new position
            }
        }

        if (m_sample_ix < NtscInputBlock::c_samples_per_video_line)
            m_sample_ix++;
        else {
            m_sample_ix = 1;
            if (m_line != NtscInputBlock::c_total_video_lines)
                m_line++;
            else {
                m_line = 1;
                if (m_state == eLocked)
                    return true;
            }
        }
    }
    return false;
}

void NtscFrameReader::setUnlocked() {
    m_state = eSearching;
    m_line = 333; // make sure we do not recognize old data and immediately re-lock
    m_log.info(eInput, "state externally set to eSearching");
}

int NtscFrameReader::expectedPulseSamplesForHalfLine(int line, int halfLine) {
    switch (line) {
        case 1:
        case 2:
        case 3:
        case 7:
        case 8:
        case 9:
        case 264:
        case 265:
        case 270:
        case 271:
            return c_equalizationPulseSamples;
        case 4:
        case 5:
        case 6:
        case 267:
        case 268:
            return c_broadPulseSamples;
        case 263:
            return halfLine == 0 ? c_normalPulseSamples : c_equalizationPulseSamples;
        case 266:
            return halfLine == 0 ? c_equalizationPulseSamples : c_broadPulseSamples;
        case 269:
            return halfLine == 0 ? c_broadPulseSamples : c_equalizationPulseSamples;
        case 272:
            return halfLine == 0 ? c_equalizationPulseSamples : c_normalPulseSamples;
        default:
            throw std::runtime_error("Table only for lines in vertical blanking");
    }
}
