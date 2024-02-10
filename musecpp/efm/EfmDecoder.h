//
// Created by staffanu on 2/8/24.
//

#ifndef MUSECPP_EFMDECODER_H
#define MUSECPP_EFMDECODER_H

#include <array>
#include "AudioDecoder.h"
#include "ReedSolomon.h"
class Logger;

class EfmDecoder {
public:
    static const size_t c_max_output_samples = 2048; // FIXME: make one in total for MUSE+EFM?

    explicit EfmDecoder(Logger &log);
    ~EfmDecoder();

    // output samples are written to the first two channels
    void decode(int frame_no, const std::vector<bool> &data, size_t &sample_count, AudioDecoder::AudioFrame output_samples[c_max_output_samples]);

private:
    static const std::array<ByteWithErasureFlag, 1 << 14> c_efm_to_byte_table;
    static std::array<ByteWithErasureFlag, 1 << 14> makeEfmInversionTable();
    static const std::array<std::pair<int, bool>, 32> c_initial_delays;
    static std::array<std::pair<int, bool>, 32> makeInitialDelays();
    static const std::array<int, 28> c_c1_to_c2_delays;
    static std::array<int, 28> makeC1ToC2Delays();
    static const std::array<int, 24> c_output_delays;
    static std::array<int, 24> makeOutputDelays();

    static const std::array<std::pair<int, int>, 6> c_left_output_map;
    static const std::array<std::pair<int, int>, 6> c_right_output_map;

    void handleFrame(size_t &sample_count, AudioDecoder::AudioFrame output_samples[2048]);

    Logger &m_log;
    int m_total_bits;
    int m_shift_register;
    int m_bit_index;
    int m_byte_index;
    int m_bits_since_sync;
    int m_consecutive_syncs;
    bool m_locked;
    int m_consecutive_sync_failures; // if not at the exact expected place

    ByteWithErasureFlag m_frame[33]; // first byte is the control data

    ReedSolomon<0x11d, 2> m_c1;
    ReedSolomon<0x11d, 2> m_c2;
    std::array<ByteWithErasureFlag *, 32> m_initial_delay_lines;
    std::array<int, 32> m_initial_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 28> m_c1_to_c2_delay_lines;
    std::array<int, 28> m_c1_to_c2_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 24> m_output_delay_lines;
    std::array<int, 24> m_output_delay_lines_ix;

    long m_total_time_us;
};

#endif //MUSECPP_EFMDECODER_H
