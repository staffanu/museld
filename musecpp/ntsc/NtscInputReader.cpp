//
// Created by staffanu on 6/30/23.
//

#include <fcntl.h>
#include <unistd.h>
#include <format>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "NtscInputReader.h"
#include "NtscRfDemodulator.h"
#include "util/Logger.h"

using namespace std;

NtscInputReader::NtscInputReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        const std::string &filename, double sample_rate,
        double initial_seek_seconds, bool benchmark_shaders,
        const std::optional<std::string> &output_filename)
        : InputReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_efm_pll(log, 40e6 / NtscRfDemodulatorConstants::c_efm_decimation_rate),
          m_file_fd(-1),
          m_sample_rate(sample_rate / NtscRfDemodulatorConstants::c_video_decimation_rate),
          m_input_samples_decimation_rate(1),
          m_demodulator(nullptr),
          m_input_buffer(nullptr),
          m_input_dropout_buffer(nullptr),
          m_input_sub_buffer_input_offsets{},
          m_last_input_sub_buffer_ix_read(0),
          m_t(0.0),
          m_state(eSearching),
          m_half_line_pulse_error_sum(0.f),
          m_half_line_pulse_error_sum_search_first(0.f),
          m_line(1),
          m_sample_ix(1),
          m_lower_percentile_filter(NtscInputBlock::c_samples_per_video_line * NtscInputBlock::c_total_video_lines, 0.005f, 0.f),
          m_consecutive_good_syncs(0),
          m_missed_line_pulses(0),
          m_frame_start_offset(0L),
          m_sample_history{},
          m_sample_history_ix(0),
          m_error_sum(0) {

    m_demodulator = new NtscRfDemodulator(log, executable_dir, m_filename, sample_rate, vulkan_manager, benchmark_shaders);
    // m_sample_rate = 31.25e6;

    m_bytes_per_sample = 4;
    m_output_multiplier = 1.0;
    m_output_add = 0.0;
}

bool NtscInputReader::initialize(std::vector<std::unique_ptr<NtscInputBlock>> &buffers) {
    if (m_demodulator == nullptr) {
        m_file_fd = open(m_filename.c_str(), O_NONBLOCK);
        if (m_file_fd == -1)
            throw runtime_error(std::format("NtscInputReader: Unable to open input file {}", m_filename));
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

    m_log.debug(eInput, std::format("NtscInputReader: m_g1={:.5f} m_g2={:.7f}", m_g1, m_g2));

    m_input_buffer = (uint8_t *)calloc(m_bytes_per_sample, c_input_buffer_size);
    m_input_dropout_buffer = (uint8_t *)calloc(1, c_input_buffer_size);
    assert(m_input_buffer != nullptr && m_input_dropout_buffer != nullptr);

    return InputReader::initialize(buffers);
}

void NtscInputReader::cleanup() {
    InputReader::cleanup();

    free(m_input_buffer);
    free(m_input_dropout_buffer);

    if (m_demodulator != nullptr) {
        m_demodulator->cleanup();
        delete m_demodulator;
    }

    if (m_file_fd != -1)
        close(m_file_fd);
}

void NtscInputReader::seek(double seconds) {
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

void NtscInputReader::threadFunc() {
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
                m_log.info(eInput, "NtscInputReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data_size = 0;;
        }

        if (!process(output_block)) {
            m_log.info(eInput, "NtscInputReader: end of file");
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

bool NtscInputReader::readInput(std::unique_ptr<NtscInputBlock> const &output_block) {
    uint8_t *read_ptr = m_input_buffer + m_bytes_per_sample * c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;
    uint8_t *dropout_read_ptr = m_input_dropout_buffer + c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;

    m_input_samples_decimation_rate = NtscRfDemodulatorConstants::c_video_decimation_rate;
    auto block = m_demodulator->getNextDemodulatedBlock();
    if (block == nullptr) {
        m_log.info(eInput, "NtscInputReader: no more demodulated blocks");
        return false;
    }
    memcpy(read_ptr, block->video_data->data<float>(), NtscRfDemodulatorConstants::c_video_block_size * sizeof(float));
    memcpy(dropout_read_ptr, block->dropouts->data<uint8_t>(), NtscRfDemodulatorConstants::c_video_block_size * sizeof(uint8_t));
    m_input_sub_buffer_input_offsets[m_last_input_sub_buffer_ix_read] = block->input_offset;

    if (output_block != nullptr) {
        if (m_state == eLocked) {
            int actual_output_size = m_efm_pll.reclock(block->efm_data->data<float>(),
                                                       NtscRfDemodulatorConstants::c_efm_block_size,
                                                       output_block->efm_data.data() + output_block->efm_data_size,
                                                       NtscInputBlock::c_max_efm_data_size - output_block->efm_data_size);
            output_block->efm_data_size += actual_output_size;
            assert(output_block->efm_data_size <= output_block->c_max_efm_data_size);
        } else
            output_block->efm_data_size = 0;
    }

    m_demodulator->returnBlock(block);

    // int fd = open("videodata.floats",
    //               O_WRONLY | O_TRUNC | O_CREAT,
    //               S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    // write(fd, read_ptr, NtscRfDemodulatorConstants::c_video_block_size * sizeof(float));
    // close(fd);

    return true;
}

bool NtscInputReader::resample(float *sample_out, uint8_t *dropout_out,
                               double input_samples_per_sample,
                               std::unique_ptr<NtscInputBlock> const &output_block) {

    m_t += input_samples_per_sample;

    const size_t read_pos = (size_t)m_t & c_input_buffer_size_mask;
    const auto i3 = (read_pos - 3) & c_input_buffer_size_mask;
    const auto i2 = (read_pos - 2) & c_input_buffer_size_mask;
    const auto i1 = (read_pos - 1) & c_input_buffer_size_mask;
    const auto i0 = read_pos;

    double b0, b1, b2, b3;
    b0 = ((float *)m_input_buffer)[i3];
    b1 = ((float *)m_input_buffer)[i2];
    b2 = ((float *)m_input_buffer)[i1];
    b3 = ((float *)m_input_buffer)[i0];

    double x = 1 + (m_t - (unsigned long)m_t);
    // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
    double a0 = b0;
    double a1 = b1 - a0;
    double a2 = (b2 - a0 - 2 * a1) / 2;
    double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
    // now evaluate at point 1 + p
    double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

    *sample_out = (float)(y * m_output_multiplier + m_output_add);
    *dropout_out = m_input_dropout_buffer[read_pos];

    auto current_sub_buffer_index = read_pos >> c_input_sub_buffer_size_bits;
    if ((m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers != current_sub_buffer_index) {
        m_last_input_sub_buffer_ix_read = (m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers;

        if (!readInput(output_block))
            return false;
    }
    return true;
}


bool NtscInputReader::process(std::unique_ptr<NtscInputBlock> const &output_block) {
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

        if (m_state == eLocked && (m_line <= 9 || m_line >= 264 && m_line <= 271)) {
            int expectedPulseSamples = expectedPulseSamplesForHalfLine(m_line, m_sample_ix >= NtscInputBlock::c_samples_per_video_line / 2);
            int halfLineSampleIx = m_sample_ix % (NtscInputBlock::c_samples_per_video_line / 2);
            float expectedVoltage = halfLineSampleIx <= expectedPulseSamples ? 0.f : 0.3f;
            m_half_line_pulse_error_sum += pow(sample - expectedVoltage, 2.f);

            if (m_sample_ix == NtscInputBlock::c_samples_per_video_line) {
                if (m_half_line_pulse_error_sum < 0.07 * NtscInputBlock::c_samples_per_video_line) {
                    m_missed_line_pulses = 0;
                } else {
                    if (m_missed_line_pulses < 3)
                        m_missed_line_pulses += 1;
                    else {
                        m_log.warn(eInput, std::format("Missed line pulses: New state eSearching at line {}", m_line));
                        m_state = eSearching;
                    }
                }
            }
        }
        if (m_state == eLockedHoriz) {
            // Looking for line 4
            int expectedPulseSamples = expectedPulseSamplesForHalfLine(4, m_sample_ix >= NtscInputBlock::c_samples_per_video_line / 2);
            int halfLineSampleIx = m_sample_ix % (NtscInputBlock::c_samples_per_video_line / 2);
            float expectedVoltage = halfLineSampleIx <= expectedPulseSamples ? 0.f : 0.3f;
            m_half_line_pulse_error_sum_search_first += pow(sample - expectedVoltage, 2.f);
            if (m_sample_ix == NtscInputBlock::c_samples_per_video_line) {
                if (m_half_line_pulse_error_sum_search_first < 0.05 * NtscInputBlock::c_samples_per_video_line) {
                    m_log.info(eInput, std::format("New state eLocked at line {}", m_line));
                    m_state = eLocked;
                    m_line = 4;
                    // FIXME: in the MUSE version, we copy the first four lines to to make the frame complete
                }
            }
        }
        if (m_sample_ix == NtscInputBlock::c_samples_per_video_line) {
            m_half_line_pulse_error_sum = 0;
            m_half_line_pulse_error_sum_search_first = 0;
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

void NtscInputReader::setUnlocked() {
    m_state = eSearching;
    m_line = 333; // make sure we do not recognize old data and immediately re-lock
    m_log.info(eInput, "state externally set to eSearching");
}

int NtscInputReader::expectedPulseSamplesForHalfLine(int line, int halfLine) {
    const double normalPulseTime = 4.7e-6;
    const double equalizationPulseTime = normalPulseTime / 2;
    const double broadPulseTime =  NtscInputBlock::c_samples_per_video_line / NtscInputBlock::c_video_sampling_frequency - normalPulseTime;

    const int equalizationPulseSamples = equalizationPulseTime * NtscInputBlock::c_video_sampling_frequency;
    const int broadPulseSamples = broadPulseTime * NtscInputBlock::c_video_sampling_frequency;

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
            return equalizationPulseSamples;
        case 4:
        case 5:
        case 6:
        case 267:
        case 268:
            return broadPulseSamples;
        case 266:
            return halfLine == 0 ? equalizationPulseSamples : broadPulseSamples;
        case 269:
            return halfLine == 0 ? broadPulseSamples : equalizationPulseSamples;
        default:
            throw std::runtime_error("Impossible case");
    }
}
