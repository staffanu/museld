//
// Created by staffanu on 2/8/24.
//

#include <fmt/format.h>
#include <cassert>
#include "EfmDecoder.h"

using namespace std;

std::array<ByteWithErasureStatus, 1 << 14> EfmDecoder::makeEfmInversionTable() {
    auto byte_to_efm = std::array<int, 256>{
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

    auto dist = [](int a, int b) -> int {
        vector<int> a_bits;
        vector<int> b_bits;
        for (int i = 0; i < 14; i++) {
            if (a & (1 << i))
                a_bits.push_back(i);
            if (b & (1 << i))
                b_bits.push_back(i);
        }
        if (a_bits.size() == b_bits.size()) {
            int sum = 0;
            for (int i = 0; i < a_bits.size(); i++)
                sum += abs(a_bits[i] - b_bits[i]);
            return sum;
        } else
            return 1000;
    };

    std::array<ByteWithErasureStatus, 1 << 14> table{};

    for (int i = 0; i < table.size(); i++) {
        int nearest = 0;
        int nearest_distance = 1000;
        for (int j = 0; j < 256; j++) {
            auto d = dist(i, byte_to_efm[j]);
            if (d < nearest_distance) {
                nearest_distance = d;
                nearest = j;
            }
        }
        if (nearest_distance >= 2)
            table[i] = ByteWithErasureStatus{0, true};
        else
            table[i] = ByteWithErasureStatus{(uint8_t)nearest, false};
    }

    return table;
};
std::array<ByteWithErasureStatus, 1 << 14> EfmDecoder::c_efm_to_byte_table = makeEfmInversionTable();

std::array<std::pair<int, bool>, 32> EfmDecoder::makeInitialDelays() {
    std::array<std::pair<int, bool>, 32> delays{};
    for (int i = 0; i < 32; i++)
        delays[i] = make_pair(i % 2, i >= 12 && i < 16 || i >= 28);
    return delays;
}
std::array<std::pair<int, bool>, 32> EfmDecoder::c_initial_delays = makeInitialDelays();

std::array<int, 28> EfmDecoder::makeC1ToC2Delays() {
    std::array<int, 28> delays{};
    for (int i = 0; i < 28; i++)
        delays[i] = (27 - i) * 4;
    return delays;
}
std::array<int, 28> EfmDecoder::c_c1_to_c2_delays = makeC1ToC2Delays();

std::array<int, 24> EfmDecoder::makeOutputDelays() {
    std::array<int, 24> delays{};
    for (int i = 0; i < 24; i++)
        delays[i] = i >= 12 ? 2 : 0;
    return delays;
}
std::array<int, 24> EfmDecoder::c_output_delays = makeOutputDelays();

std::array<std::pair<int, int>, 6> EfmDecoder::c_left_output_map =
        array<pair<int, int>, 6> { pair{0, 1}, pair{12, 13}, pair{2, 3}, pair{14, 15}, pair{4, 5}, pair{16, 17}};
std::array<std::pair<int, int>, 6> EfmDecoder::c_right_output_map =
        array<pair<int, int>, 6> { pair{6, 7}, pair{18, 19}, pair{8, 9}, pair{20, 21}, pair{10, 11}, pair{22, 23}};

EfmDecoder::EfmDecoder(Logger &log)
        : m_log(log),
          m_total_bits(0),
          m_shift_register(0),
          m_bit_index(0),
          m_byte_index(33),
          m_bits_since_sync(0),
          m_consecutive_syncs(0),
          m_locked(false),
          m_consecutive_sync_failures(0), // if not at the exact expected place
          m_frame{},
          m_c1(),
          m_c2(),
          m_initial_delay_lines{},
          m_initial_delay_lines_ix{},
          m_c1_to_c2_delay_lines{},
          m_c1_to_c2_delay_lines_ix{},
          m_output_delay_lines{},
          m_output_delay_lines_ix{}
{
    for (int i = 0; i < m_initial_delay_lines.size(); i++)
        m_initial_delay_lines[i] = new ByteWithErasureStatus[c_initial_delays[i].first + 1];
    for (int i = 0; i < m_c1_to_c2_delay_lines.size(); i++)
        m_c1_to_c2_delay_lines[i] = new ByteWithErasureStatus[c_c1_to_c2_delays[i] + 1];
    for (int i = 0; i < m_output_delay_lines.size(); i++)
        m_output_delay_lines[i] = new ByteWithErasureStatus[c_output_delays[i] + 1];
}

EfmDecoder::~EfmDecoder() {
    for (auto p: m_initial_delay_lines)
        delete[] p;
    for (auto p: m_c1_to_c2_delay_lines)
        delete[] p;
    for (auto p: m_output_delay_lines)
        delete[] p;
}

void EfmDecoder::decode(const vector<bool> &data, size_t &sample_count, AudioDecoder::AudioFrame output_samples[c_max_output_samples]) {
    sample_count = 0;

    for (auto bool_bit: data) {
        m_total_bits += 1;
        int bit = bool_bit ? 1 : 0;
        m_shift_register = m_shift_register << 1 | bit;
        m_bits_since_sync += 1;
        bool sync = (m_shift_register & 0xffffff) == 0x801002; // 1000 0000 0001 0000 0000 0010

        if (sync && (m_bits_since_sync == 588 || !m_locked)) { // expected -- all fine!
            m_bit_index = 0;
            m_byte_index = 0;
            m_bits_since_sync = 0;
            m_consecutive_sync_failures = 0;
            m_consecutive_syncs += 1;
            if (m_consecutive_syncs >= 3 && !m_locked) {
                m_locked = true;
                m_log.debug(eAudio, fmt::format("efm m_locked index {}", m_total_bits));
            }
        } else if (m_bits_since_sync == 588 && m_locked) {
            m_bit_index = 0;
            m_byte_index = 0;
            m_bits_since_sync = 0;
            m_consecutive_sync_failures += 1;
            m_consecutive_syncs = 0;
            if (m_consecutive_sync_failures >= 7 && m_locked) {
                m_locked = false;
                m_log.debug(eAudio, fmt::format("efm lock lost index {}", m_total_bits));
            }
        } else if (m_byte_index < 33) {
            if (m_byte_index > 0) {
                if (m_bit_index == 16) {
                    int efm_value = m_shift_register & 0x3fff; // 14 bits
                    ByteWithErasureStatus octet = c_efm_to_byte_table[efm_value];
                    m_frame[m_byte_index] = octet;

                    if (m_byte_index == 32) {
                        handleFrame(sample_count, output_samples);
                    }
                }
            }
            if (m_bit_index == 16) {
                m_bit_index = 0;
                m_byte_index = m_byte_index + 1;
            } else
                m_bit_index = m_bit_index + 1;
        }
    }
}

void EfmDecoder::handleFrame(size_t &sample_count, AudioDecoder::AudioFrame output_samples[2048]) {
    ByteWithErasureStatus c1_in[32];

    for (int i = 0; i < 32; i++) {
        m_initial_delay_lines[i][m_initial_delay_lines_ix[i]++] = m_frame[i + 1];
        if (m_initial_delay_lines_ix[i] == c_initial_delays[i].first + 1)
            m_initial_delay_lines_ix[i] = 0;
        auto v = m_initial_delay_lines[i][m_initial_delay_lines_ix[i]];
        c1_in[i] = c_initial_delays[i].second ? v : ByteWithErasureStatus{(uint8_t)~v.value, v.erased};
    }

    ByteWithErasureStatus c1_out[28];
    m_c1.decode(c1_in, c1_out);

    ByteWithErasureStatus c2_in[28];

    for (int i = 1; i < 28; i++) {
        m_c1_to_c2_delay_lines[i][m_c1_to_c2_delay_lines_ix[i]++] = c1_out[i];
        if (m_c1_to_c2_delay_lines_ix[i] == c_c1_to_c2_delays[i] + 1)
            m_c1_to_c2_delay_lines_ix[i] = 0;
        c2_in[i] = m_c1_to_c2_delay_lines[i][m_c1_to_c2_delay_lines_ix[i]];
    }

    ByteWithErasureStatus c2_out[24];
    m_c2.decode(c2_in, c2_out);

    ByteWithErasureStatus out[24];

    for (int i = 1; i < 24; i++) {
        m_output_delay_lines[i][m_output_delay_lines_ix[i]++] = c2_out[i];
        if (m_output_delay_lines_ix[i] == c_output_delays[i] + 1)
            m_output_delay_lines_ix[i] = 0;
        out[i] = m_output_delay_lines[i][m_output_delay_lines_ix[i]];
    }

    for (int i = 0; i < 6; i++) {
        auto [lh, ll] = c_left_output_map[i];
        output_samples[sample_count].samples[0] = (int16_t) ((out[lh].value << 8) | out[ll].value);

        auto [rh, rl] = c_right_output_map[i];
        output_samples[sample_count].samples[1] = (int16_t) ((out[rh].value << 8) | out[rl].value);

        sample_count++;
        assert(sample_count <= c_max_output_samples);
    }
}
