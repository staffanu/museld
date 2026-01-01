//
// Created by staffanu on 2/8/24.
//

#include <format>
#include <cassert>
#include "EfmDecoder.h"

using namespace std;

const std::array<uint16_t, 256> EfmDecoder::c_byte_to_efm_toggles_table {
    0x1220, 0x2100, 0x2420, 0x2220, 0x1100, 0x0110, 0x0420, 0x0900,
    0x1240, 0x2040, 0x2440, 0x2240, 0x1040, 0x0040, 0x0440, 0x0840,
    0x2020, 0x2080, 0x2480, 0x0820, 0x1080, 0x0080, 0x0480, 0x0880,
    0x1210, 0x2010, 0x2410, 0x2210, 0x1010, 0x0210, 0x0410, 0x0810,
    0x0020, 0x2108, 0x0220, 0x0920, 0x1108, 0x0108, 0x1020, 0x0908,
    0x1248, 0x2048, 0x2448, 0x2248, 0x1048, 0x0048, 0x0448, 0x0848,
    0x0100, 0x2088, 0x2488, 0x2110, 0x1088, 0x0088, 0x0488, 0x0888,
    0x1208, 0x2008, 0x2408, 0x2208, 0x1008, 0x0208, 0x0408, 0x0808,
    0x1224, 0x2124, 0x2424, 0x2224, 0x1124, 0x0024, 0x0424, 0x0924,
    0x1244, 0x2044, 0x2444, 0x2244, 0x1044, 0x0044, 0x0444, 0x0844,
    0x2024, 0x2084, 0x2484, 0x0824, 0x1084, 0x0084, 0x0484, 0x0884,
    0x1204, 0x2004, 0x2404, 0x2204, 0x1004, 0x0204, 0x0404, 0x0804,
    0x1222, 0x2122, 0x2422, 0x2222, 0x1122, 0x0022, 0x1024, 0x0922,
    0x1242, 0x2042, 0x2442, 0x2242, 0x1042, 0x0042, 0x0442, 0x0842,
    0x2022, 0x2082, 0x2482, 0x0822, 0x1082, 0x0082, 0x0482, 0x0882,
    0x1202, 0x0248, 0x2402, 0x2202, 0x1002, 0x0202, 0x0402, 0x0802,
    0x1221, 0x2121, 0x2421, 0x2221, 0x1121, 0x0021, 0x0421, 0x0921,
    0x1241, 0x2041, 0x2441, 0x2241, 0x1041, 0x0041, 0x0441, 0x0841,
    0x2021, 0x2081, 0x2481, 0x0821, 0x1081, 0x0081, 0x0481, 0x0881,
    0x1201, 0x2090, 0x2401, 0x2201, 0x1090, 0x0201, 0x0401, 0x0890,
    0x0221, 0x2109, 0x1110, 0x0121, 0x1109, 0x0109, 0x1021, 0x0909,
    0x1249, 0x2049, 0x2449, 0x2249, 0x1049, 0x0049, 0x0449, 0x0849,
    0x0120, 0x2089, 0x2489, 0x0910, 0x1089, 0x0089, 0x0489, 0x0889,
    0x1209, 0x2009, 0x2409, 0x2209, 0x1009, 0x0209, 0x0409, 0x0809,
    0x1120, 0x2111, 0x2490, 0x0224, 0x1111, 0x0111, 0x0490, 0x0911,
    0x0241, 0x2101, 0x0244, 0x0240, 0x1101, 0x0101, 0x0090, 0x0901,
    0x0124, 0x2091, 0x2491, 0x2120, 0x1091, 0x0091, 0x0491, 0x0891,
    0x1211, 0x2011, 0x2411, 0x2211, 0x1011, 0x0211, 0x0411, 0x0811,
    0x1102, 0x0102, 0x2112, 0x0902, 0x1112, 0x0112, 0x1022, 0x0912,
    0x2102, 0x2104, 0x0249, 0x0242, 0x1104, 0x0104, 0x0422, 0x0904,
    0x0122, 0x2092, 0x2492, 0x0222, 0x1092, 0x0092, 0x0492, 0x0892,
    0x1212, 0x2012, 0x2412, 0x2212, 0x1012, 0x0212, 0x0412, 0x0812
};

std::array<ByteWithErasureFlag, 1 << 14> EfmDecoder::makeEfmInversionTable() {

    std::array<ByteWithErasureFlag, 1 << 14> table{};
    table.fill(ByteWithErasureFlag(0, true));

    for (int i = 0; i < 256; i++)
        table[c_byte_to_efm_toggles_table[i]] = ByteWithErasureFlag(i, false);

    return table;
}
const std::array<ByteWithErasureFlag, 1 << 14> EfmDecoder::c_efm_toggles_to_byte_table = makeEfmInversionTable();

std::array<uint16_t, 256> EfmDecoder::makeEfmChannelSymbolTable() {
    std::array<uint16_t, 256> table{};

    for (int i = 0; i < 256; i++) {
        int toggles = c_byte_to_efm_toggles_table[i];
        int seq = 0; // i as symbol sequence (from toggles) assume start with 0
        int prev = 0;
        for (int k = 0; k < 14; k++) {
            if (toggles & 1 << (13 - k))
                prev = 1 - prev;
            seq = seq << 1 | prev;
        }
        table[i] = seq;
    }

    return table;
}

const std::array<uint16_t, 256> EfmDecoder::c_byte_to_channel_symbols_table = makeEfmChannelSymbolTable();

std::array<std::pair<int, bool>, 32> EfmDecoder::makeInitialDelays() {
    std::array<std::pair<int, bool>, 32> delays{};
    for (int i = 0; i < 32; i++)
        delays[i] = make_pair(i % 2, i >= 12 && i < 16 || i >= 28);
    return delays;
}
const std::array<std::pair<int, bool>, 32> EfmDecoder::c_initial_delays = makeInitialDelays();

std::array<int, 28> EfmDecoder::makeC1ToC2Delays() {
    std::array<int, 28> delays{};
    for (int i = 0; i < 28; i++)
        delays[i] = (27 - i) * 4;
    return delays;
}
const std::array<int, 28> EfmDecoder::c_c1_to_c2_delays = makeC1ToC2Delays();

std::array<int, 24> EfmDecoder::makeOutputDelays() {
    std::array<int, 24> delays{};
    for (int i = 0; i < 24; i++)
        delays[i] = i >= 12 ? 2 : 0;
    return delays;
}
const std::array<int, 24> EfmDecoder::c_output_delays = makeOutputDelays();

const std::array<std::pair<int, int>, 6> EfmDecoder::c_left_output_map =
        array<pair<int, int>, 6> { pair{0, 1}, pair{12, 13}, pair{2, 3}, pair{14, 15}, pair{4, 5}, pair{16, 17}};
const std::array<std::pair<int, int>, 6> EfmDecoder::c_right_output_map =
        array<pair<int, int>, 6> { pair{6, 7}, pair{18, 19}, pair{8, 9}, pair{20, 21}, pair{10, 11}, pair{22, 23}};

EfmDecoder::EfmDecoder(Logger &log)
        : m_log(log),
          m_total_bits(0),
          m_prev_symbol(0.f),
          m_shift_register(0),
          m_bit_index(0),
          m_byte_index(33),
          m_bits_in_frame(0),
          m_consecutive_syncs(0),
          m_locked(false),
          m_frames_since_sync(0), // if not at the exact expected place
          m_efm_frames_since_lock(0),
          m_subcode_symbol_index(-3),
          m_frame{},
          m_subcode_p_start_flag(false),
          m_subcode_q_pre_emphasis(nullopt),
          m_c1(32, 28, 0, true, false),
          m_c2(28, 24, 0, true, false),
          m_initial_delay_lines{},
          m_initial_delay_lines_ix{},
          m_c1_to_c2_delay_lines{},
          m_c1_to_c2_delay_lines_ix{},
          m_output_delay_lines{},
          m_output_delay_lines_ix{},
          m_efm_frame_count_last_second(0),
          m_total_time_us_last_second(0),
          m_total_erasures_in_last_second(0),
          m_total_erasures_past_c1_last_second(0),
          m_total_erasures_out_last_second(0)
{
    for (int i = 0; i < m_initial_delay_lines.size(); i++)
        m_initial_delay_lines[i] = new ByteWithErasureFlag[c_initial_delays[i].first + 1];
    for (int i = 0; i < m_c1_to_c2_delay_lines.size(); i++)
        m_c1_to_c2_delay_lines[i] = new ByteWithErasureFlag[c_c1_to_c2_delays[i] + 1];
    for (int i = 0; i < m_output_delay_lines.size(); i++)
        m_output_delay_lines[i] = new ByteWithErasureFlag[c_output_delays[i] + 1];
}

EfmDecoder::~EfmDecoder() {
    for (auto p: m_initial_delay_lines)
        delete[] p;
    for (auto p: m_c1_to_c2_delay_lines)
        delete[] p;
    for (auto p: m_output_delay_lines)
        delete[] p;
}

void EfmDecoder::decode(const std::vector<float> &data, std::vector<TwoChannelSampleWithErasureFlags> &output_samples, bool log_now) {
    auto t0 = chrono::high_resolution_clock::now();

    output_samples.clear();
    for (float symbol : data) {
        bool toggle = symbol * m_prev_symbol < 0;
        m_total_bits += 1;
        m_bits_in_frame += 1;

        m_shift_register = m_shift_register << 1 | toggle;

        // Keep the last 15 symbols to do ML estimation if no perfect match
        m_symbol_history.push_back(symbol);
        if (m_symbol_history.size() > 15)
            m_symbol_history.pop_front();

        bool new_frame = false;
        bool sync = (m_shift_register & 0xffffff) == 0x801002; // 1000 0000 0001 0000 0000 0010

        if (sync) {
            m_frames_since_sync = 0;
            m_consecutive_syncs += 1; // this is not right -- we increment consecutive_syncs even if not in an expected position
            if (m_consecutive_syncs >= 7 && !m_locked) {
                m_locked = true;
                m_efm_frames_since_lock = 0;
                m_log.debug(eAudio, std::format("efm locked index {}", m_total_bits));
            }
            new_frame = true;
        } else if (m_bits_in_frame == 588) {
            m_frames_since_sync += 1;
            m_consecutive_syncs = 0;
            if (m_frames_since_sync >= 7 && m_locked) {
                m_locked = false;
                m_log.debug(eAudio, std::format("efm lock lost index {}", m_total_bits));
                if (m_efm_frames_since_lock > c_minimum_frames_before_c1_c2_valid)
                    m_log.debug(eAudio, reedSolomonStatistics());
            }
            new_frame = true;
        }

        if (new_frame) {
            m_bit_index = 0;
            m_byte_index = 0;
            m_bits_in_frame = 0;
        } else if (m_byte_index < 33) {
            if (m_bit_index == 16) {
                int efm_value = m_shift_register & 0x3fff; // 14 bits
                ByteWithErasureFlag octet = c_efm_toggles_to_byte_table[efm_value];
                m_frame[m_byte_index] = octet;
                if (octet.isErased()) {
                    m_total_erasures_in_last_second++;
                    m_frame[m_byte_index] = ByteWithErasureFlag(estimateSymbol(), true);
                }

                if (m_byte_index == 0) {
                    if (efm_value == 0b00100000000001)
                        m_subcode_symbol_index = -2;
                    else if (efm_value == 0b00000000010010 && m_subcode_symbol_index == -2)
                        m_subcode_symbol_index = -1;
                    else if (m_subcode_symbol_index < 95)
                        m_subcode_symbol_index++;
                    else
                        m_subcode_symbol_index = -3;
                }

                if (m_byte_index == 32) {
                    handleFrame(output_samples);
                    handleSubcode();
                    m_efm_frame_count_last_second++;
                }
                m_bit_index = 0;
                m_byte_index++;
            } else
                m_bit_index++;
        }
        m_prev_symbol = symbol;
    }

    auto t1 = chrono::high_resolution_clock::now();
    m_total_time_us_last_second += chrono::duration_cast<chrono::microseconds>(t1 - t0).count();

    if (log_now) {
        m_log.info(eAudio | ePerformance,
                   std::format("Time spent decoding last second: {:.1f} ms; efm frame count: {}, "
                               "input erasure rate: {:.0f} ppm, past c1 erasure rate: {:.0f} ppm, output erasure rate: {:.0f} ppm",
                               (double)m_total_time_us_last_second / 1000.0,
                               m_efm_frame_count_last_second,
                               1000000.0 * m_total_erasures_in_last_second / m_efm_frame_count_last_second / 33,
                               1000000.0 * m_total_erasures_past_c1_last_second / m_efm_frame_count_last_second / 28,
                               1000000.0 * m_total_erasures_out_last_second / m_efm_frame_count_last_second / 24));
        m_efm_frame_count_last_second = 0;
        m_total_time_us_last_second = 0;
        m_total_erasures_in_last_second = 0;
        m_total_erasures_past_c1_last_second = 0;
        m_total_erasures_out_last_second = 0;
    }
}

std::string EfmDecoder::reedSolomonStatistics() {
    return "C1 statistics: " + m_c1.statistics() + "C2 statistics: " + m_c2.statistics();
}

void EfmDecoder::handleSubcode() {
    if (m_subcode_symbol_index < 0)
        return;
    m_subcode[m_subcode_symbol_index] = m_frame[0];
    if (m_subcode_symbol_index < 95)
        return;

    // we have collected a complete 96 bit subcode

    // Handle subcode P
    for (int i = 0; i < 96; i++) {
        if ((m_subcode[i].byteValue() & 1) != m_subcode_p_start_flag) {
            m_subcode_p_start_flag = m_subcode[i].byteValue() & 1;
            // TODO: check for consecutive values, but this is not used; might implement for fun or when needed
            // m_log.debug(eAudio, std::format("Subcode P start flag = {}", m_subcode_p_start_flag));
        }
    }

    // Handle subcode Q

    uint16_t crc = 0x0000;
    for (int i = 0; i < 96; i++) {
        uint8_t inbit = (m_subcode[i].byteValue() & 2) != 0;

        uint8_t msb = (crc >> 15) & 1;
        crc <<= 1;
        crc &= 0xFFFF;

        if (msb ^ inbit) {
            crc ^= 0x1021;
        }
    }

    if (crc == 0) {
        // Pre-emphasis
        if ((m_subcode[0].byteValue() & 2) == 0 && (m_subcode[1].byteValue() & 2) == 0) {
            bool pre_emphasis = m_subcode[3].byteValue() & 2;
            if (!m_subcode_q_pre_emphasis.has_value() || m_subcode_q_pre_emphasis.value() != pre_emphasis) {
                m_subcode_q_pre_emphasis = pre_emphasis;
                m_log.debug(eAudio, std::format("Subcode Q pre-emphasis = {}", pre_emphasis));
            }
        }
    } else {
        //m_log.debug(eAudio, "Subcode Q CRC check failed");
    }
}

// ML estimation of the received symbol -- used only when we do not have a perfect match
uint8_t EfmDecoder::estimateSymbol() const {
    double max_p = 0;
    uint8_t found = 0;
    for (int i = 0; i < 256; i++) {
        const int symbol_sequence = c_byte_to_channel_symbols_table[i];
        const int seqs[2] = {symbol_sequence, ~symbol_sequence}; // two possibilities
        for (auto seq: seqs) {
            double p_tot = 1.0;
            for (int j = 0; j < 15; j++) {
                auto v = m_symbol_history[j];
                double d = (1 << (14 - j)) & seq ? 1.0 : -1.0;
                double p = d > 0 ? (v > 0.5 ? 1.0 : std::max(0.0, v + 0.5)) : (v < -0.5 ? 1.0 : std::max(0.0, 0.5 - v));
                p_tot *= p;
            }
            if (p_tot > max_p) {
                max_p = p_tot;
                found = i;
            }
        }
    }
    return found;
}

void EfmDecoder::handleFrame(std::vector<TwoChannelSampleWithErasureFlags> &output_samples) {
    std::vector<ByteWithErasureFlag> c1_data(32);

    for (int i = 0; i < 32; i++) {
        m_initial_delay_lines[i][m_initial_delay_lines_ix[i]++] = m_frame[i + 1];
        if (m_initial_delay_lines_ix[i] == c_initial_delays[i].first + 1)
            m_initial_delay_lines_ix[i] = 0;
        auto v = m_initial_delay_lines[i][m_initial_delay_lines_ix[i]];
        c1_data[i] = c_initial_delays[i].second ? ByteWithErasureFlag{(uint8_t)~v.byteValue(), v.isErased()} : v;
    }

    m_c1.decode(c1_data);

    std::vector<ByteWithErasureFlag> c2_data(28);

    for (int i = 0; i < 28; i++) {
        m_c1_to_c2_delay_lines[i][m_c1_to_c2_delay_lines_ix[i]++] = c1_data[i];
        if (m_c1_to_c2_delay_lines_ix[i] == c_c1_to_c2_delays[i] + 1)
            m_c1_to_c2_delay_lines_ix[i] = 0;
        c2_data[i] = m_c1_to_c2_delay_lines[i][m_c1_to_c2_delay_lines_ix[i]];
        if (c2_data[i].isErased())
            m_total_erasures_past_c1_last_second++;
    }

    m_c2.decode(c2_data);

    ByteWithErasureFlag out[24];

    for (int i = 0; i < 24; i++) {
        m_output_delay_lines[i][m_output_delay_lines_ix[i]++] = c2_data[i < 12 ? i : i + 4];
        if (m_output_delay_lines_ix[i] == c_output_delays[i] + 1)
            m_output_delay_lines_ix[i] = 0;
        out[i] = m_output_delay_lines[i][m_output_delay_lines_ix[i]];
        if (out[i].isErased())
            m_total_erasures_out_last_second++;
    }

    for (int i = 0; i < 6; i++) {
        TwoChannelSampleWithErasureFlags sample{};

        auto [lh, ll] = c_left_output_map[i];
        bool lErased = out[lh].isErased() || out[ll].isErased();

        sample.samples[0] = (int16_t) ((out[lh].byteValue() << 8) | out[ll].byteValue());
        sample.erased[0] = lErased;

        auto [rh, rl] = c_right_output_map[i];
        bool rErased = out[rh].isErased() || out[rl].isErased();

        sample.samples[1] = (int16_t) ((out[rh].byteValue() << 8) | out[rl].byteValue());
        sample.erased[1] = rErased;

        output_samples.push_back(sample);
    }

    m_efm_frames_since_lock++;
    if (m_efm_frames_since_lock < c_minimum_frames_before_c1_c2_valid) {
        m_c1.resetStatistics();
        m_c2.resetStatistics();
    }
}
