// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <format>
#include <cassert>
#include "BchDecoder.h"
#include "logging/Logger.h"

BchDecoder::BchDecoder(int n, int primitive_polynomial, bool correct_two_errors)
        : m_n(n),
          m_primitive_polynomial(primitive_polynomial),
          m_correct_two_errors(correct_two_errors),
          m_highest_bit_in_primitive_polynomial(0),
          m_statistics(),
          m_alpha_powers(),
          m_logs() {
    m_highest_bit_in_primitive_polynomial = 1;
    int tmp = primitive_polynomial;
    while (tmp >>= 1)
        m_highest_bit_in_primitive_polynomial <<= 1;

    m_alpha_powers.resize(m_highest_bit_in_primitive_polynomial - 1);
    m_logs.resize(m_highest_bit_in_primitive_polynomial);

    int alpha_pow = 1;
    for (int i = 0; i < m_highest_bit_in_primitive_polynomial - 1; i++) {
        m_alpha_powers[i] = alpha_pow;
        m_logs[alpha_pow] = i;
        alpha_pow = alpha_pow << 1;
        if (alpha_pow & m_highest_bit_in_primitive_polynomial)
            alpha_pow ^= m_primitive_polynomial;
    }
}

int BchDecoder::alphaPower(int a) {
    return m_alpha_powers[a % (m_highest_bit_in_primitive_polynomial - 1)];
}

int BchDecoder::log(int a) {
    assert(a != 0);
    return m_logs[a];
}

int BchDecoder::inv(int a) {
    assert(a != 0);
    if (a == 1)
        return 1;
    else
        return m_alpha_powers[m_highest_bit_in_primitive_polynomial - 1 - m_logs[a]];
}

int BchDecoder::multiply(int a, int b) {
    if (a == 0 || b == 0)
        return 0;
    return alphaPower(log(a) + log(b));
}

void BchDecoder::assertSyndromesZero(int bits[]) {
#ifndef NDEBUG
    int s0 = 0;
    int s1 = 0;
    int s3 = 0;
    for (int i = 0; i < m_n; i++) {
        s0 ^= bits[m_n - i - 1]; // The input has the msb first, so reverse indices
        s1 ^= bits[m_n - i - 1] * alphaPower(i);
        if (m_correct_two_errors)
            s3 ^= bits[m_n - i - 1] * alphaPower(2 * i);
    }
    assert(s0 == 0 && s1 == 0 && s3 == 0);
#endif
}

bool BchDecoder::decode(int bits[]) {
    // In a binary code, s2 = s1^2 and s4 = s2^2, so we do not compute s2 and s4 since they add no information.
    // s1 and s3 are enough to solve for two error locations, and the errors themselves can have only one value.
    // Notice that s0, which is always 0 or 1, is not really part of the BCH code -- it is just a parity sum
    // that is always even, and is how the DEC distance 5 code is converted into a DEC-TED code (or SEC into SEC-DED).
    int s0 = 0;
    int s1 = 0;
    int s3 = 0;
    for (int i = 0; i < m_n; i++) {
        s0 ^= bits[m_n - i - 1]; // The input has the msb first, so reverse indices
        s1 ^= bits[m_n - i - 1] * alphaPower(i);
        if (m_correct_two_errors)
            s3 ^= bits[m_n - i - 1] * alphaPower(3 * i);
    }
    // If we are only correcting one error, set s3 to s1^3, which it is in the cases of 0 or 1 error.
    if (!m_correct_two_errors)
        s3 = multiply(s1, multiply(s1, s1));

    if (s0 == 0 && s1 == 0 && s3 == 0) {
        m_statistics["input ok"]++;
        return true;
    } else if (s1 !=0 && s3 != 0) {
        int s1pow3 = multiply(s1, multiply(s1, s1));
        if (s1pow3 == s3 && s0 == 1) { // one error if determinant is zero; need odd parity
            int logS1 = log(s1);
            if (logS1 < m_n) {
                bits[m_n - logS1 - 1] ^= 1;
                m_statistics["corrected 1 error"]++;
                assertSyndromesZero(bits);
                return true;
            } else {
                m_statistics["single outside range"]++;
                return false;
            }
        } else if (s1pow3 != s3 && s0 == 0) { // two errors if non-zero determinant; need even parity
            int lambda1 = s1; // for a binary code it simplifies to this
            int lambda2 = multiply(s1, s1) ^ multiply(s3, inv(s1));

            std::vector<int> roots;
            auto alphaPowI = 1;
            auto lambda1TimesAlphaPowI = lambda1; // for i == 0
            for (int i = 0; i < m_n; i++) {
                if ((lambda1TimesAlphaPowI ^ multiply(alphaPowI, alphaPowI)) == lambda2)
                    roots.push_back(i);
                alphaPowI = multiply(alphaPowI, 2);
                lambda1TimesAlphaPowI = multiply(lambda1TimesAlphaPowI, 2);
            }

            if (roots.size() > 2) {
                m_statistics["too many roots"]++;
                return false;
            } else if (roots.size() != 2) {
                m_statistics["dual outside range"]++;
                return false;
            } else {
                int pos1 = roots[0];
                int pos2 = roots[1];
                assert(pos1 <= m_n && pos2 <= m_n);
                bits[m_n - pos1 - 1] ^= 1;
                bits[m_n - pos2 - 1] ^= 1;
                m_statistics["corrected 2 errors"]++;
                assertSyndromesZero(bits);
                return true;
            }
        } else {
            m_statistics["too many errors"]++;
            return false;
        }
    } else {
        m_statistics["inconsistent syndromes"]++;
        return false;
    }
}

void BchDecoder::resetStatistics() {
    m_statistics.clear();
}

void BchDecoder::printStatistics(Logger &log, std::string const &message) {
    std::ostringstream ss;
    ss << message << ": ";
    for (const auto &el: m_statistics)
        ss << el.first << ": " << el.second << ", ";
    log.info(eAudio, ss.str());
}
