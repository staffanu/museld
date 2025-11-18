//
// Created by staffanu on 2/8/24.
//

#ifndef MUSECPP_EFMDECODER_H
#define MUSECPP_EFMDECODER_H

#include <array>
//#include "InputReader.h"
#include "TwoChannelSample.h"
#include "../rs/ReedSolomon.h"
//#include "InputBlockBase.h"

class Logger;

class EfmDecoder {
public:
    explicit EfmDecoder(Logger &log);
    ~EfmDecoder();

    EfmDecoder(const EfmDecoder &) = delete;
    EfmDecoder &operator=(const EfmDecoder &) = delete;
    EfmDecoder(EfmDecoder &&) = delete;
    EfmDecoder &operator=(EfmDecoder &&) = delete;

    // output samples are written to the first two channels
    void decode(const bool data[], int input_data_size,
        int max_output_samples, int *sample_count, TwoChannelSample *output_samples,
        bool log_now);

    std::string reedSolomonStatistics();

private:
    static constexpr int c_minimum_frames_before_c1_c2_valid = 97;

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

    void handleFrame(int max_output_samples, int &sample_count, TwoChannelSample output_samples[2048]);

    Logger &m_log;
    int m_total_bits;
    int m_shift_register;
    int m_bit_index;
    int m_byte_index;
    int m_bits_since_sync;
    int m_consecutive_syncs;
    bool m_locked;
    int m_consecutive_sync_failures; // if not at the exact expected place
    int m_efm_frames_since_lock;

    std::array<ByteWithErasureFlag, 33> m_frame; // first byte is the control data

    ReedSolomon<0x11d, 2> m_c1;
    ReedSolomon<0x11d, 2> m_c2;
    std::array<ByteWithErasureFlag *, 32> m_initial_delay_lines;
    std::array<int, 32> m_initial_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 28> m_c1_to_c2_delay_lines;
    std::array<int, 28> m_c1_to_c2_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 24> m_output_delay_lines;
    std::array<int, 24> m_output_delay_lines_ix;

    int m_efm_frame_count_last_second;
    long m_total_time_us_last_second;
    int m_total_erasures_in_last_second;
    int m_total_erasures_past_c1_last_second;
    int m_total_erasures_out_last_second;
};

#endif //MUSECPP_EFMDECODER_H
