//
// Created by Staffan Ulfberg on 6/16/23.
//

#ifndef MUSECPP_BCHDECODER_H
#define MUSECPP_BCHDECODER_H

#include <map>
#include <vector>
#include <iostream>

/** BCH SEC-DED decoder.  We assume that the distance d of the code is 4.
 *
 * @param n the codeword length
 * @param k the number of information symbols; there are n-k bits for error correction
 * @param generator FIXME rename
 * @param primPoly the primitive polynomial
 *
 * The degree of the primitive polynomial is m.
 * We have n <= 2 ** m - 1
 * Assume n > 2 ** (m - 1) - 1 => 2 ** (m - 1) < n + 1 <= 2 ** m => m - 1 < log2(n+1) <= m => m = ceil(log2(n+1))
 *
 * The generator polynomial has degree k.
 */
class BchDecoder {
public:
    BchDecoder(int n, int k, int generator):
    m_n(n),
    m_k(k),
    m_generator(generator) {
        // Notice we only compute the powers up to n - 1.  This means that in the case of
        // multiple errors the log could fail, but we cannot correct in that case anyway.
        // If we make the log table complete we need to check if the log is in range for shortened codes.
        int alpha_pow = 1;
        for (int i = 0; i < n; i++) {
            m_alpha_powers.push_back(alpha_pow);
            m_logs[alpha_pow] = i;
            if ((alpha_pow & (1 << (n - k - 1 - 1))) != 0)
                alpha_pow = (alpha_pow << 1) ^ generator;
            else
                alpha_pow = alpha_pow << 1;
        }
    }

    void resetStatistics() {
        m_statistics.clear();
    }

    void printStatistics() {
        for (auto el: m_statistics)
            std::cout << el.first << ": " << el.second << std::endl;
    }

    // bits are left to right with msb first -- we therefore index bit i (0 based) as bits(n - i - 1)
    bool decode(int bits[]) {
        int s0 = 0;
        int s1 = 0;
        for (int i = 0; i < m_n; i++) {
            s0 ^= bits[m_n - i - 1];
            s1 ^= bits[m_n - i - 1] * m_alpha_powers[i];
        }
        if (s0 == 0 && s1 == 0) {
            m_statistics["input ok"]++;
            return true;
        } else if (s0 != 0) {
            auto it = m_logs.find(s1);
            if (it == m_logs.end()) {
                m_statistics["cannot correct bad syndrome"]++;
                return false;
            } else {
                int logS1 = it->second;
                bits[m_n - logS1 - 1] ^= 1;
                m_statistics["corrected 1 error"]++;
                return true;
            }
        } else {
            m_statistics["cannot correct two errors"]++;
            return false;
        }
    }

private:
    int m_n;
    int m_k;
    int m_generator;
    std::map<std::string, int> m_statistics;
    std::vector<int> m_alpha_powers;
    std::map<int, int> m_logs;
};

#endif //MUSECPP_BCHDECODER_H
