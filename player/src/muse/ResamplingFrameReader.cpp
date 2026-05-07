//
// Created by staffanu on 6/30/23.
//

#include <fcntl.h>
#include <unistd.h>
#include <format>
#include <cassert>
#include <filesystem>
#include "musevk/VulkanBuffer.h"
#include "ResamplingFrameReader.h"
#include "input/InputReaderFactory.h"
#include "logging/Logger.h"
#include "MuseConstants.h"
#include "MuseInputBlock.h"

using namespace std;

ResamplingFrameReader::ResamplingFrameReader(
        Logger &log, const std::string &executable_dir, musevk::VulkanManager &vulkan_manager,
        const std::string &filename, InputFormat input_format, double sample_rate,
        double initial_seek_seconds, bool demodulate, bool benchmark_shaders,
        const std::optional<std::string> &output_filename)
        : FrameReader(log, filename,
                      filesystem::is_fifo(filename),
                      initial_seek_seconds, output_filename),
          m_input_format(input_format),
          m_demodulate(demodulate),
          m_input_reader(nullptr),
          m_sample_rate(demodulate ? sample_rate / MuseDemodulatedBlock::c_video_decimation_rate : sample_rate),
          m_input_samples_decimation_rate(1),
          m_demodulator(nullptr),
          m_input_buffer(nullptr),
          m_input_dropout_buffer(nullptr),
          m_input_sub_buffer_input_offsets{},
          m_last_input_sub_buffer_ix_read(0),
          m_t(0.0),
          m_input_scale(1.f),
          m_input_offset(0.f),
          m_state(eSearching),
          m_line(1),
          m_pixel(1),
          m_line1_frame_pulse_sum(0),
          m_line2_frame_pulse_sum(0),
          m_upper_percentile_filter(MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT, 0.995f, 239.f),
          m_lower_percentile_filter(MUSE_TOTAL_WIDTH * MUSE_TOTAL_HEIGHT, 0.005f, 16.f),
          m_max_frame_pulse_sum_difference(0),
          m_consecutive_good_syncs(0),
          m_missed_line_pulses(0),
          m_frame_start_offset(0L),
          m_error_sum(0) {
    if (m_demodulate) {
        m_demodulator = new MuseRfDemodulator(log, executable_dir, m_filename, (float)sample_rate, vulkan_manager,
                                              m_input_format, benchmark_shaders);
        // The demodulator already scales output to MUSE 0..255 range.
        m_input_scale = 1.f;
        m_input_offset = 0.f;
    } else {
        // File mode: scale incoming float samples (which carry the native value range of the input
        // format) to the ~0..255 range that downstream sync detection expects.
        switch (m_input_format) {
            case eUint8:
                m_input_scale = 1.f;
                m_input_offset = 0.f;
                break;
            case eSint8:
                m_input_scale = 1.f;
                m_input_offset = 128.f;
                break;
            case eUint16:
            case eUint16BE:
                m_input_scale = 1.f / 256.f;
                m_input_offset = 0.f;
                break;
            case eSint16:
            case eSint16BE:
                m_input_scale = 1.f / 256.f;
                m_input_offset = 128.f;
                break;
            case eLds:
                m_input_scale = 1.f / 4.f;
                m_input_offset = 0.f;
                break;
            case eFlac:
            case eFlacOgg:
                // Bit depth only known after initialize(); deferred to initialize().
                m_input_scale = 0.f;
                m_input_offset = 0.f;
                break;
            default:
                throw runtime_error(std::format("ResamplingFrameReader: unsupported input format {}", (int)m_input_format));
        }
    }
}

bool ResamplingFrameReader::initialize(std::vector<std::unique_ptr<MuseInputBlock>> &buffers) {
    if (m_demodulator == nullptr) {
        m_input_reader = makeInputReader(m_filename, m_input_format, c_input_sub_buffer_size);
        m_input_reader->initialize();
        if (m_input_scale == 0.f) {
            int bps = m_input_reader->bitsPerSample();
            if (bps == 8) {
                m_input_scale = 1.f;
                m_input_offset = 128.f;
            } else if (bps == 16) {
                m_input_scale = 1.f / 256.f;
                m_input_offset = 128.f;
            } else {
                throw runtime_error(std::format("ResamplingFrameReader: unexpected FLAC bits per sample {}", bps));
            }
        }
    } else {
        m_demodulator->initialize(MuseDemodulatedBlock::recommendedNumberOfBlockBuffers((float)m_sample_rate * MuseDemodulatedBlock::c_video_decimation_rate));
    }

    m_input_samples_per_sample_ref = m_sample_rate / 16.2e6;
    m_input_samples_per_sample = m_input_samples_per_sample_ref;
    m_omega = 2 * M_PI * 3000 / m_sample_rate;
    m_zeta = 0.75;
    m_Ts = m_input_samples_per_sample_ref * 480;
    m_G1 = 1 - exp(-2 * m_zeta * m_omega * m_Ts);
    m_G2 = 1 + exp(-2 * m_omega * m_zeta * m_Ts) -
           2 * exp(-m_omega * m_zeta * m_Ts) * cos(m_omega * m_Ts * sqrt(1 - m_zeta * m_zeta));
    m_GpdGvco = 64 * (1 / m_input_samples_per_sample_ref) * 480;
    m_g1 = m_G1 / m_GpdGvco;
    m_g2 = m_G2 / m_GpdGvco;

    m_log.debug(eInput, std::format("m_g1={:.5f} m_g2={:.7f}", m_g1, m_g2));

    m_input_buffer = (float *)calloc(sizeof(float), c_input_buffer_size);
    m_input_dropout_buffer = (uint8_t *)calloc(1, c_input_buffer_size);
    assert(m_input_buffer != nullptr && m_input_dropout_buffer != nullptr);

    return FrameReader::initialize(buffers);
}

void ResamplingFrameReader::cleanup() {
    FrameReader::cleanup();

    free(m_input_buffer);
    free(m_input_dropout_buffer);

    if (m_demodulator != nullptr) {
        m_demodulator->cleanup();
        delete m_demodulator;
    }

    m_input_reader.reset();
}

void ResamplingFrameReader::seek(double seconds) {
    if (!m_input_is_realtime) {
        if (m_demodulator != nullptr) {
            m_demodulator->seek(seconds);
        } else {
            std::unique_lock<std::mutex> lock(m_mutex);

            off_t samples_to_seek = (off_t) (seconds * 16.2e6 * m_input_samples_per_sample);
            m_log.info(eInput, std::format("Seeking relative time {} s, {} samples.",
                                           seconds, samples_to_seek));
            m_input_reader->seek(samples_to_seek);

            // discard content in existing input buffers
            move(m_filled_input_buffers.begin(), m_filled_input_buffers.end(),
                 back_inserter(m_vacant_input_buffers));
            m_filled_input_buffers.clear();
            m_cv_vacant.notify_one();
        }
        setUnlocked(); // do not wait to discover that we lost sync
    }
}

void ResamplingFrameReader::threadFunc() {
    unique_ptr<MuseInputBlock> output_block = nullptr;

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
                m_log.info(eInput, "ResamplingFrameReader: stop requested");
                break;
            }
            output_block = std::move(m_vacant_input_buffers.front());
            m_vacant_input_buffers.pop_front();
            output_block->efm_data.clear();
        }

        if (!process(output_block)) {
            m_log.info(eInput, "ResamplingFrameReader: end of file");
            break;
        }

        output_block->input_offset = m_frame_start_offset;
        output_block->input_samples_per_muse_sample = m_input_samples_per_sample * m_input_samples_decimation_rate;
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_filled.notify_one();
        m_filled_input_buffers.push_back(std::move(output_block));
        output_block = nullptr;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv_filled.notify_one();
    m_reader_thread_finished = true;
}

bool ResamplingFrameReader::readInput(std::unique_ptr<MuseInputBlock> const &output_block) {
    float *read_ptr = m_input_buffer + c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;
    uint8_t *dropout_read_ptr = m_input_dropout_buffer + c_input_sub_buffer_size * m_last_input_sub_buffer_ix_read;

    if (m_demodulator == nullptr) {
        m_input_samples_decimation_rate = 1;
        if (m_input_reader->readFloats(read_ptr) != (int)c_input_sub_buffer_size) {
            if (!m_input_is_realtime) {
                m_log.info(eInput, "ResamplingFrameReader: end of file");
                return false;
            }
            return true;
        }
        if (m_input_scale != 1.f || m_input_offset != 0.f) {
            for (size_t i = 0; i < c_input_sub_buffer_size; i++)
                read_ptr[i] = read_ptr[i] * m_input_scale + m_input_offset;
        }
        return true;
    } else {
        m_input_samples_decimation_rate = MuseDemodulatedBlock::c_video_decimation_rate;
        auto block = m_demodulator->getNextDemodulatedBlock();
        if (block == nullptr) {
            m_log.info(eInput, "ResamplingFrameReader: no more demodulated blocks");
            return false;
        }
        memcpy(read_ptr, block->video_data->data<float>(), MuseDemodulatedBlock::c_video_block_size * sizeof(float));
        memcpy(dropout_read_ptr, block->dropouts->data<uint8_t>(), MuseDemodulatedBlock::c_video_block_size * sizeof(uint8_t));
        m_input_sub_buffer_input_offsets[m_last_input_sub_buffer_ix_read] = block->input_offset;

        if (output_block != nullptr) {
            if (m_state == eLocked)
                output_block->efm_data.insert(output_block->efm_data.end(), block->efm_data.begin(), block->efm_data.end());
            else
                output_block->efm_data.clear();
        }

        m_demodulator->returnBlock(block);
        return true;
    }
}

bool ResamplingFrameReader::resample(float *sample_out, uint8_t *dropout_out,
                                     double input_samples_per_sample,
                                     std::unique_ptr<MuseInputBlock> const &output_block) {

    m_t += input_samples_per_sample;

    const size_t read_pos = (size_t)m_t & c_input_buffer_size_mask;
    const auto i3 = (read_pos - 3) & c_input_buffer_size_mask;
    const auto i2 = (read_pos - 2) & c_input_buffer_size_mask;
    const auto i1 = (read_pos - 1) & c_input_buffer_size_mask;
    const auto i0 = read_pos;

    const double b0 = m_input_buffer[i3];
    const double b1 = m_input_buffer[i2];
    const double b2 = m_input_buffer[i1];
    const double b3 = m_input_buffer[i0];

    double x = 1 + (m_t - (unsigned long)m_t);
    // cubic spline though 4 points, f(x) = a0 + a1 x + a2 x(x-1) + a3 x(x-1)(x-2)
    double a0 = b0;
    double a1 = b1 - a0;
    double a2 = (b2 - a0 - 2 * a1) / 2;
    double a3 = (b3 - a0 - 3 * a1 - 6 * a2) / 6;
    // now evaluate at point 1 + p
    double y = a0 + a1 * x + a2 * x * (x - 1) + a3 * x * (x - 1) * (x - 2);

    *sample_out = (float)y;
    *dropout_out = m_input_dropout_buffer[read_pos];

    auto current_sub_buffer_index = read_pos >> c_input_sub_buffer_size_bits;
    if ((m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers != current_sub_buffer_index) {
        m_last_input_sub_buffer_ix_read = (m_last_input_sub_buffer_ix_read + 1) % c_number_of_input_sub_buffers;

        if (!readInput(output_block))
            return false;
    }
    return true;
}


bool ResamplingFrameReader::process(std::unique_ptr<MuseInputBlock> const &output_block) {
    auto *output = output_block->video_data->data<float>();
    auto *dropout_output = output_block->dropout_data->data<uint8_t>();

    m_frame_start_offset = m_input_sub_buffer_input_offsets[((size_t)m_t & c_input_buffer_size_mask) >> c_input_sub_buffer_size_bits];

    float sample;
    uint8_t dropout;
    while (resample(&sample, &dropout, m_input_samples_per_sample, output_block)) {
        m_upper_percentile_filter.update(sample);
        m_lower_percentile_filter.update(sample);

        int output_index = MUSE_TOTAL_WIDTH * (m_line - 1) + m_pixel - 1;
        output[output_index] = sample;
        dropout_output[output_index] = dropout;

        if (m_state == eLockedHoriz || m_state == eLocked && m_line <= 2) {
            if (m_pixel == 312) {
                m_line1_frame_pulse_sum = m_line2_frame_pulse_sum;
                m_line2_frame_pulse_sum = 0;
            } else if (m_pixel >= 313 && m_pixel <= 478) { // skip the two last pixels: especially line 2 often has bad values
                int pulsePixel = m_pixel - 313;
                float pulseValue = (pulsePixel < 144 ? pulsePixel / 4 % 2 == 0 : pulsePixel < 160) ? 1.f : -1.f;
                m_line2_frame_pulse_sum += pulseValue * sample;
            }
            // We calculate the sums over 164 pixels, so if the signal levels of the high and low parts of the signal
            // differ by d, a perfect fit with the frame sync signals should have a difference between line 1 and line 2
            // of 166 * d.  If the input is scaled according to the MUSE specification, d = 239 - 16 = 223,
            // so the difference should be 37018.  In practice, for a correctly scaled input signal we get around 27000,
            // or 73 % of the theoretical value.  The percentile filters estimate the upper/lower signal values, but there
            // is some variation due to the content of the rest of the signal, so we set the thresholds lower.
            if (m_pixel == 480) {
                if (m_state == eLockedHoriz) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    m_max_frame_pulse_sum_difference = max(m_max_frame_pulse_sum_difference, m_line1_frame_pulse_sum - m_line2_frame_pulse_sum);
                    if (m_line1_frame_pulse_sum - m_line2_frame_pulse_sum > threshold) {

                        m_log.info(eInput, std::format("New state eLocked at line {}, diff={}, threshold={}",
                                                       m_line, m_line1_frame_pulse_sum - m_line2_frame_pulse_sum, threshold));
                        // copy the two last lines to lines 1 and 2 so that the first frame is complete -- ignore dropouts
                        for (int i = 0; i < MUSE_TOTAL_WIDTH; i++) {
                            output[i] = output[(m_line - 2) * MUSE_TOTAL_WIDTH + i];
                            output[i + MUSE_TOTAL_WIDTH] = output[(m_line - 1) * MUSE_TOTAL_WIDTH + i];
                        }
                        m_line = 2;
                        m_state = eLocked;

                        m_frame_start_offset = m_input_sub_buffer_input_offsets[((size_t)m_t & c_input_buffer_size_mask) >> c_input_sub_buffer_size_bits]
                                - MUSE_TOTAL_WIDTH * 2 - 1;
                    }
                }
                if (m_state == eLocked && m_line == 2) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    if (m_line1_frame_pulse_sum - m_line2_frame_pulse_sum > threshold)
                        m_missed_line_pulses = 0;
                    else {
                        if (m_missed_line_pulses < 3)
                            m_missed_line_pulses += 1;
                        else {
                            m_log.warn(eInput, std::format("Missed line pulses (diff={}, threshold={}): New state eSearching at line {}",
                                                           m_line1_frame_pulse_sum - m_line2_frame_pulse_sum, threshold, m_line));
                            m_state = eSearching;
                        }
                    }
                }
            }
        }

        if (m_pixel == 8) {
            // When locked: line 1: positive, line 2: negative, line 3: negative, m_line 4: positive, then alternating up to 1125
            bool sync_should_be_positive = m_line == 1 || (m_line > 3 && m_line % 2 == 0);

            float sample0 = output[output_index - 4];
            float sample2 = output[output_index - 2];
            float sample4 = output[output_index];

            bool m_sync_is_good = (sync_should_be_positive && sample0 < sample2 && sample2 < sample4) ||
                                  (!sync_should_be_positive && sample0 > sample2 && sample2 > sample4);

            if (m_sync_is_good) {
                double avgLevel = (sample0 + sample4) / 2;
                double new_error = std::clamp(sync_should_be_positive ? sample2 - avgLevel : avgLevel - sample2, -10000.0, 10000.0); // negative means we sampled too early
                m_error_sum = std::clamp(m_error_sum + new_error, -15000.0, 15000.0);
                m_input_samples_per_sample =
                        m_input_samples_per_sample_ref - new_error * m_g1 - m_error_sum * m_g2;
            }
            if (m_state == eSearching) {
                m_consecutive_good_syncs = m_sync_is_good ? m_consecutive_good_syncs + 1 :
                                           m_consecutive_good_syncs >= 2 ? m_consecutive_good_syncs - 2 : 0;
                if (m_consecutive_good_syncs >= 50) {
                    m_log.info(eInput, std::format("New state eLockedHoriz at line {}", m_line));
                    m_state = eLockedHoriz;
                    m_line2_frame_pulse_sum = 0;
                    m_max_frame_pulse_sum_difference = 0;
                }
            }
            if ((m_state == eSearching && ((m_consecutive_good_syncs < 5 && m_line == 50) || m_line == 100)) ||
                (m_state == eLockedHoriz && m_line == 1125)) {
                m_consecutive_good_syncs = 0;
                m_error_sum = 0;
                m_input_samples_per_sample = m_input_samples_per_sample_ref;

                if (m_state == eLockedHoriz) {
                    float threshold = 223.0f * 0.25f * (m_upper_percentile_filter.getEstimate() - m_lower_percentile_filter.getEstimate());
                    m_log.info(eInput, std::format(
                            "Locked horizontally, but frame pulses not found (max sum={}, threshold={}): New state eSearching at line {}",
                            m_max_frame_pulse_sum_difference, threshold, m_line));
                }
                m_state = eSearching; // TODO: add to VHDL
                m_line = 3;
                m_pixel = 263; // "random", start search from new position
            }
        }

        if (m_pixel < 480)
            m_pixel++;
        else {
            m_pixel = 1;
            if (m_line != 1125)
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

void ResamplingFrameReader::setUnlocked() {
    m_state = eSearching;
    m_line = 333; // make sure we do not recognize old data and immediately re-lock
    m_log.info(eInput, "state externally set to eSearching");
}
