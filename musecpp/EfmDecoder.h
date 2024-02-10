//
// Created by staffanu on 2/8/24.
//

#ifndef MUSECPP_EFMDECODER_H
#define MUSECPP_EFMDECODER_H

#include <array>
#include "AudioDecoder.h"
class Logger;

struct ByteWithErasureStatus {
    uint8_t value;
    bool erased;
};

class EfmDecoder {
public:
    static const size_t c_max_output_samples = 2048; // FIXME: make one in total for MUSE+EFM?

    explicit EfmDecoder(Logger &log);
    ~EfmDecoder();

    // output samples are written to the first two channels
    void decode(const std::vector<bool> &data, size_t &sample_count, AudioDecoder::AudioFrame output_samples[c_max_output_samples]);

private:
    class CircDecoder {
    public:
        void resetStatistics() {}
    };
    class C1Decoder : public CircDecoder {
    public:
        void decode(ByteWithErasureStatus in[32], ByteWithErasureStatus out[28]) {
            for (int i = 0; i < 28; i++)
                out[i] = in[i];
        }
    private:
    };
    class C2Decoder : public CircDecoder {
    public:
        void decode(ByteWithErasureStatus in[28], ByteWithErasureStatus out[24]) {
            for (int i = 0; i < 12; i++)
                out[i] = in[i];
            for (int i = 12; i < 24; i++)
                out[i] = in[i + 4];
        }
    private:
    };

    static std::array<ByteWithErasureStatus, 1 << 14> c_efm_to_byte_table;
    static std::array<ByteWithErasureStatus, 1 << 14> makeEfmInversionTable();
    static std::array<std::pair<int, bool>, 32> c_initial_delays;
    static std::array<std::pair<int, bool>, 32> makeInitialDelays();
    static std::array<int, 28> c_c1_to_c2_delays;
    static std::array<int, 28> makeC1ToC2Delays();
    static std::array<int, 24> c_output_delays;
    static std::array<int, 24> makeOutputDelays();

    static std::array<std::pair<int, int>, 6> c_left_output_map;
    static std::array<std::pair<int, int>, 6> c_right_output_map;

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

    ByteWithErasureStatus m_frame[33]; // first byte is the control data

    C1Decoder m_c1;
    C2Decoder m_c2;
    std::array<ByteWithErasureStatus *, 32> m_initial_delay_lines;
    std::array<int, 32> m_initial_delay_lines_ix;
    std::array<ByteWithErasureStatus *, 28> m_c1_to_c2_delay_lines;
    std::array<int, 28> m_c1_to_c2_delay_lines_ix;
    std::array<ByteWithErasureStatus *, 24> m_output_delay_lines;
    std::array<int, 24> m_output_delay_lines_ix;
};

#endif //MUSECPP_EFMDECODER_H
