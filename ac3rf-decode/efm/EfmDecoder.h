//
// Created by staffanu on 2/8/24.
//

#ifndef MUSECPP_EFMDECODER_H
#define MUSECPP_EFMDECODER_H

#include <array>
#include <deque>
#include <span>

#include "TimingRecovery.h"
#include "TwoChannelSample.h"
#include "../rs/ReedSolomon.h"

class Logger;

class EfmDecoder {
public:
    explicit EfmDecoder(Logger &log, std::optional<std::string> circ_debug_filename_opt = std::nullopt);
    ~EfmDecoder();

    EfmDecoder(const EfmDecoder &) = delete;
    EfmDecoder &operator=(const EfmDecoder &) = delete;
    EfmDecoder(EfmDecoder &&) = delete;
    EfmDecoder &operator=(EfmDecoder &&) = delete;

    std::vector<TwoChannelSampleWithErasureFlags> decode(const std::vector<float> &data, bool log_now);
    std::string reedSolomonStatistics();

private:
    static constexpr int c_minimum_frames_before_c1_c2_valid = 97;

    static const std::array<uint16_t, 256> c_byte_to_efm_toggles_table;
    static const std::array<ByteWithErasureFlag, 1 << 14> c_efm_toggles_to_byte_table;
    static std::array<ByteWithErasureFlag, 1 << 14> makeEfmInversionTable();

    static const std::array<uint16_t, 256> c_byte_to_channel_symbols_table;
    static std::array<uint16_t, 256> makeEfmChannelSymbolTable();

    static const std::array<std::pair<int, bool>, 32> c_initial_delays;
    static std::array<std::pair<int, bool>, 32> makeInitialDelays();
    static const std::array<int, 28> c_c1_to_c2_delays;
    static std::array<int, 28> makeC1ToC2Delays();
    static const std::array<int, 24> c_output_delays;
    static std::array<int, 24> makeOutputDelays();

    static const std::array<std::pair<int, int>, 6> c_left_output_map;
    static const std::array<std::pair<int, int>, 6> c_right_output_map;

    void dumpArray(std::string const &prefix, std::span<ByteWithErasureFlag> const &data);
    void handleFrame(std::vector<TwoChannelSampleWithErasureFlags> &output_samples);
    void handleSubcode();

    uint8_t estimateSymbol() const;

    Logger &m_log;
    FILE *m_circ_debug_file;
    int m_total_bits;
    float m_prev_symbol;
    int m_shift_register;
    std::deque<float> m_symbol_history;
    int m_bit_index;
    int m_byte_index;
    int m_bits_in_frame;
    int m_consecutive_syncs;
    bool m_locked;
    int m_frames_since_sync; // if not at the exact expected place
    int m_efm_frames_since_lock;

    // -3 -- 95; the symbol itself is in m_frame[0] for indices 0--95
    // -3 means that we didn't see the sync sequence S0, S1
    // -2, -1 means that we are in the sync sequence
    int m_subcode_symbol_index;
    std::array<ByteWithErasureFlag, 33> m_frame; // first byte is the control data

    // bits 7-0 correspond to P-W
    std::array<ByteWithErasureFlag, 96> m_subcode;
    bool m_subcode_p_start_flag;
    enum SubcodePUse {
        Audio2ChannelNoPreEmphasis,
        Audio2ChannelWithPreEmphasis,
        DigitalData,
        Broadcasting
    };
    std::optional<SubcodePUse> m_subcode_q_use;

    ReedSolomon<0x11d, 2> m_c1;
    ReedSolomon<0x11d, 2> m_c2;
    std::array<ByteWithErasureFlag *, 32> m_initial_delay_lines;
    std::array<int, 32> m_initial_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 28> m_c1_to_c2_delay_lines;
    std::array<int, 28> m_c1_to_c2_delay_lines_ix;
    std::array<ByteWithErasureFlag *, 24> m_output_delay_lines;
    std::array<int, 24> m_output_delay_lines_ix;

    int m_total_efm_frame_count;
    int m_efm_frame_count_last_second;
    long m_total_time_us_last_second;
    int m_total_erasures_in_last_second;
    int m_total_erasures_past_c1_last_second;
    int m_total_erasures_out_last_second;
    int m_subcodes_last_second;
    int m_q_subcode_crc_failures_last_second;
};

#endif //MUSECPP_EFMDECODER_H
