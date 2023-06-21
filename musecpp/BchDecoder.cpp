//
// Created by Staffan Ulfberg on 6/16/23.
//

#include "BchDecoder.h"

BchDecoder::BchDecoder(int n, int k, int generator)
: m_n(n),
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

bool BchDecoder::decode(int bits[]) {
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