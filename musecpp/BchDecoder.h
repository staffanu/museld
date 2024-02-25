//
// Created by Staffan Ulfberg on 6/16/23.
//

#ifndef MUSECPP_BCHDECODER_H
#define MUSECPP_BCHDECODER_H

#include <map>
#include <vector>
#include <sstream>
#include "util/Logger.h"

class Logger;

/** BCH decoder.
 * if correct_two_errors is false, this is a SEC-DED decoder: We assume that the distance d of the base code is 3
 * and that one additional bit is used to ensure that all codewords have even parity.
 * If correct_two_errors is true, we correct two errors and detect three errors: We assume the distance of the
 * base code is 5, and again that one bit is used to ensure that all codewords have even parity.
 *
 * @param n the codeword length
 * @param primitive_polynomial
 * @param correct_two_errors
 * Notice that k, the number of information bits, is not needed -- the parity bits are discarded by the caller.
 * Also, the generator polynomial for the code is not actually used by the decoder: it is enough to know
 * the primitive polynomial for the Galois field for computations.
 *
 * The degree of the primitive polynomial is m.
 * We have n <= 2 ** m - 1
 */
class BchDecoder {
public:
    BchDecoder(int n, int primitive_polynomial, bool correct_two_errors);

    // In MUSE, bits are left to right with msb first, so that is what this decoder assumes.
    bool decode(int bits[]);
    void resetStatistics();
    void printStatistics(Logger &log, std::string const &message);

private:
    int alphaPower(int a);
    int log(int a);
    int inv(int a);
    int multiply(int a, int b);
    void assertSyndromesZero(int bits[]);

    int m_n;
    int m_primitive_polynomial;
    int m_correct_two_errors;
    int m_highest_bit_in_primitive_polynomial;
    std::map<std::string, int> m_statistics;
    std::vector<int> m_alpha_powers;
    std::vector<int> m_logs;
};

#endif //MUSECPP_BCHDECODER_H
