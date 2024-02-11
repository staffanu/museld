//
// Created by Staffan Ulfberg on 6/16/23.
//

#ifndef MUSECPP_BCHDECODER_H
#define MUSECPP_BCHDECODER_H

#include <map>
#include <vector>
#include <iostream>
#include <sstream>
#include "Logger.h"

class Logger;
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
    BchDecoder(int n, int k, int generator);

    // bits are left to right with msb first -- we therefore index bit i (0 based) as bits(n - i - 1)
    bool decode(int bits[]);

    void resetStatistics() {
        m_statistics.clear();
    }

    void printStatistics(Logger &log, std::string const &message) {
        std::ostringstream ss;
        ss << message << ": ";
        for (const auto &el: m_statistics)
            ss << el.first << ": " << el.second << ", ";
        log.info(eAudio, ss.str());
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
