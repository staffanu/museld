//
// Created by Staffan Ulfberg on 2/8/24.
//

#ifndef MUSECPP_REEDSOLOMON_H
#define MUSECPP_REEDSOLOMON_H

#include <vector>
#include <map>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <fmt/format.h>
#include "GFComputer.h"
#include "../Logger.h"

struct ByteWithErasureFlag {
    ByteWithErasureFlag() : value(0), erased(false) {};
    explicit ByteWithErasureFlag(int v) : value(v), erased(false) {};
    ByteWithErasureFlag(int v, bool e) : value(v), erased(e) {};
    uint8_t value;
    bool erased;
};

template<int irreducible_poly, int alpha> class ReedSolomon {
public:
    ReedSolomon(int n, int k, int fcr, bool make_corrections)
            : m_n(n),
              m_k(k),
              m_fcr(fcr),
              m_make_corrections(make_corrections),
              m_num(GFComputer<8, 0x11d, alpha>()),
              alphaInverse(m_num.inverse(alpha)),
              alphaSquared(m_num.multiply(alpha, alpha)) {
        for (int row = 0; row < n - k; row++) {
            std::vector<int> h_row;
            for (int col = 0; col < n; col++) {
                h_row.push_back(m_num.pow(m_num.pow(alpha, row + fcr), col));
            }
            H.push_back(h_row);
        }
    }

    // The first symbol in the input is the highest order coefficient
    void decode(ByteWithErasureFlag d[]);

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
    int m_fcr;
    bool m_make_corrections;
    GFComputer<8, irreducible_poly, alpha> m_num;
    int alphaInverse;
    int alphaSquared;

    std::vector<std::vector<int>> H;
    std::map<std::string, int> m_statistics;

    void doDecode(std::vector<ByteWithErasureFlag> data);

    std::vector<int> computeSyndromes(std::vector<ByteWithErasureFlag> data) {
        std::vector<int> syndromes;
        for (int i = 0; i < m_n - m_k; i++) {
            int s = 0;
            for (int j = 0; j < m_n; j++)
                s = m_num.add(s, m_num.multiply(H[i][j], data[j].value));
            syndromes.push_back(s);
        }
        return syndromes;
    }

    void eraseAll(std::vector<ByteWithErasureFlag> data) {
        for (auto b: data)
            b.erased = true;
    }

    void uneraseAll(std::vector<ByteWithErasureFlag> data) {
        for (auto b: data)
            b.erased = false;
        m_statistics["unerased all"]++;
    }

    bool tryCorrectOneError(std::vector<int> syndromes, std::vector<ByteWithErasureFlag> data) {
            if (std::count(syndromes.cbegin(), syndromes.cend(), 0) != 0) {
                m_statistics["single error some syndrome zero"]++;
                return false;
            } else {
                int x0 = m_num.multiply(syndromes[1], m_num.inverse(syndromes[0]));
                int errorPos0 = m_num.log(x0);
                int y0 = m_num.multiply(syndromes[0], m_num.inverse(m_num.pow(alpha, m_num.multiply(errorPos0, m_fcr))));

                if (errorPos0 < 0 || errorPos0 >= m_n) {
                    m_statistics["single error outside range"] += 1;
                    return false;
                } else if (syndromes[2] * m_num.inverse(syndromes[1]) != x0 ||
                           syndromes[3] * m_num.inverse(syndromes[2]) != x0) {
                    m_statistics["single error inconsistent syndromes"] += 1;
                    return false;
                } else {
                    data[errorPos0] = ByteWithErasureFlag(data[errorPos0].value + y0);
                    m_statistics["single correction count"] += 1;
                    return true;
                }
            }
    }

    bool tryCorrectTwoErrors(std::vector<int> syndromes, std::vector<ByteWithErasureFlag> data) {
            int determinant = m_num.add(m_num.multiply(syndromes[1], syndromes[1]), m_num.multiply(syndromes[0], syndromes[2]));
            if (determinant == 0) {
                m_statistics["dual determinant zero"];
                return false;
            } else {
//                int detInv = m_num.inverse(determinant);
//                int lambda1 = (syndromes[1] * syndromes[2] + syndromes[0] * syndromes[3]) * detInv;
//                int lambda2 = (syndromes[2] * syndromes[2] + syndromes[1] * syndromes[3]) * detInv;
//
//                std::vector<std::pair<int, int>> roots;
//                int alphaPowI = 1;
//                int lambda1TimesAlphaPowI = lambda1; // for i == 0
//                int lambda2TimesAlphaPow2I = 1; // we swap lambda0 (which is 1) and lambda2 in order to get the error locations directly without inversion
//                for (int i = 0; i < m_n; i++) {
//                    if (lambda1TimesAlphaPowI + lambda2TimesAlphaPow2I == lambda2)
//                        roots.emplace_back(alphaPowI, i);
//                    alphaPowI = alphaPowI * alpha;
//                    lambda1TimesAlphaPowI = lambda1TimesAlphaPowI * alpha;
//                    lambda2TimesAlphaPow2I = lambda2TimesAlphaPow2I * alphaSquared;
////        println(f"$i: ${alphaPowI.a}%02x ${alphaPowMinusI.a}%02x ${lambda1TimesAlphaPowI.a}%02x ${lambda2TimesAlphaPow2I.a}%02x")
//                }
//
//                if (roots.size() != 2) {
//                    m_statistics["dual too few roots"] += 1;
//                    return false;
//                } else {
//                    auto [x0, errorPos0] = roots[0];
//                    auto [x1, errorPos1] = roots[roots.size() - 1];
//                    assert(errorPos0 >= 0 && errorPos0 < n && errorPos1 >= 0 && errorPos1 < n);
//
//                    int x0PlusX1Inv = m_num.inverse(m_num.add(x0, x1));
//                    int y0 = m_num.add(syndromes[1], m_num.multiply(syndromes[0], x1)) * x0PlusX1Inv * m_num.inverse(m_num.pow(alpha, m_num.multiply(errorPos0, m_fcr)));
//                    int y1 = m_num.add(syndromes[1], m_num.multiply(syndromes[0], x0)) * x0PlusX1Inv * m_num.inverse(m_num.pow(alpha, m_num.multiply(errorPos1, m_fcr)));
//
//                    data[errorPos0] = ByteWithErasureFlag(data[errorPos0].value + y0);
//                    data[errorPos1] = ByteWithErasureFlag(data[errorPos1].value + y1);
//                    m_statistics["dual correction count"] += 1;
//                    return true;
//                }
                m_statistics["dual correction not implemented"] += 1;
                return false;
            }
    }

    bool tryDecodeErasures(std::vector<int> syndromes, std::vector<ByteWithErasureFlag> data) {
        std::vector<int> errorPositions;
        for (int i = 0; i < m_n; i++)
            if (data[i].erased)
                errorPositions.push_back(i);
        if (errorPositions.size() > syndromes.size()) {
            m_statistics["erasure decoding failed with ${errorPositions.size} erasures"]++;
            return false;
        } else {
            std::vector<int> x;
            std::transform(errorPositions.begin(), errorPositions.end(), x.begin(), [this](int b) -> int { return m_num.pow(alpha, b); });
            auto s = syndromes;
            s.resize(errorPositions.size());
            std::vector<int> y = solveSyndromeEquations(x, s);

            for (int i = 0; i < errorPositions.size(); i++) {
                int errorPos = errorPositions[i];
                int error = y[i];
                data[errorPos] = ByteWithErasureFlag(data[errorPos].value + error * m_num.inverse(m_num.pow(alpha, errorPos * m_fcr)));
            }
            m_statistics["corrected ${errorPositions.size} erasures count"]++;
            return true;
        }
    }

    bool tryDecodeOneErrorTwoErasures(std::vector<int> syndromes, std::vector<ByteWithErasureFlag> data) {
        std::vector<int> errorPositions;
        for (int i = 0; i < m_n; i++)
            if (data[i].erased)
                errorPositions.push_back(i);
        if (errorPositions.size() != 2) {
            m_statistics["one error two erasures needs two erasures"]++;
            return false;
        } else {
            // See "Method for correcting both errors and erasures of RS codes using error-only and erasure-only decoding algorithms"
            // by Erl-Huei Lu, Pen-Yao Lu, Tso-Cho Chen (https://doi.org/10.1049/el.2013.1521)

            // Think of y0 and y1 being the erased errors at locators x0 and x1
//            val alphaInv = alpha.inverse
//            val s0prime = syndromes[0] + syndromes[1] * pow(alphaInv, errorPositions[0])
//            val s1prime = syndromes[1] * pow(alphaInv, errorPositions[0]) +
//                          syndromes[2] * pow(alphaInv, 2 * errorPositions[0])
//            val s2prime = syndromes[2] * pow(alphaInv, 2 * errorPositions[0]) +
//                          syndromes[3] * pow(alphaInv, 3 * errorPositions[0])
//            val posDiff = errorPositions[1] - errorPositions[0] // positive
//            val s0bis = s0prime + s1prime * pow(alphaInv, posDiff)
//            val s1bis = s1prime * pow(alphaInv, posDiff) + s2prime * pow(alphaInv, 2 * posDiff)
//
//            val x2prime = s1bis * s0bis.inverse
//            val errorPos2prime = log(x2prime) // this is errorPos2 - errorPos1
//            val errorPos2 = (errorPos2prime + errorPositions[1]) %
//                            255 // Can this ever be out of range? Tried to make it happen but not so far
//            val y2prime = s0bis
//            val x0 = alphaPow(errorPositions[0])
//            val x2 = alphaPow(errorPos2)
//            val y2BeforeFcrAdjust = y2prime * (unit + x0.inverse * x2).inverse * (unit + x2prime).inverse
//            val y2 = y2BeforeFcrAdjust * pow(alpha, errorPos2 * fcr).inverse
//
//            data(errorPos2) = GF256WithValue(data(errorPos2).v + y2)
//
//            val fixedSyndromes = IndexedSeq(
//                    syndromes[0] + y2BeforeFcrAdjust,
//                    syndromes[1] + y2BeforeFcrAdjust * alphaPow(errorPos2)
//            )
//
//            val c = tryDecodeErasures(fixedSyndromes, data)
//            if (c)
//                m_statistics["single correction with two erasures"]++;
//            else
//                m_statistics["single correction with two erasures failed on erasures"]++;
//            return c;
            return false;
        }
    }

    /**
     * Solves the equation system
     *
     * x(0)^k y(0) + ... + x(n-1)^k y(n-1) = s(k), for all k = 0...n-1
     */
    std::vector<int> solveSyndromeEquations(std::vector<int> x, std::vector<int> s) {
        int m = (int)x.size();
        assert(s.size() == m);
        assert(std::count(x.cbegin(), x.cend(), 0) == 0);

        // compute the matrix by multiplying x element wise to the previous rows
        std::vector<int> row;
        row.resize(m);
        std::fill(row.begin(), row.end(), 1);

        std::vector<std::vector<int>> mat;
        mat.resize(m);
        for (int i = 0; i < m; i++) {
            mat[i] = row;
            for (int j = 0; j < m; j++)
                row[j] *= x[j];
        }

        auto y = s;
        for (int i = 0; i < m - 1; i++) { // for each row, clear column i under it
            int leadingInverse = m_num.inverse(mat[i][i]);
            for (int j = i; j < m; j++) // make first element unit
                mat[i][j] = mat[i][j] * leadingInverse;
            y[i] *= leadingInverse;

            for (int k = i + 1; k < m; k++) { // rows under
                int leading = mat[k][i];
                for (int j = i; j < m; j++)
                    mat[k][j] = mat[k][j] + leading * mat[i][j];
                y[k] = y[k] + leading * y[i];
            }
        }

        // make bottom right element unit
        int brInverse = m_num.inverse(mat[m - 1][m - 1]);
        mat[m - 1][m - 1] = mat[m - 1][m - 1] * brInverse;
        y[m - 1] *= brInverse;

        for (int i = m - 1; i > 0; i--) { // for each row, clear column i over it
            for (int k = i - 1; k >= 0; k--) { // rows over
                y[k] += m_num.multiply(mat[k][i], y[i]);
            }
        }

        return y;
    }
};

template<int irreducible_poly, int alpha>
void ReedSolomon<irreducible_poly, alpha>::decode(ByteWithErasureFlag d[]) {
    std::vector<ByteWithErasureFlag> data;
    data.resize(m_n);
    for (int i = 0; i < m_n; i++)
        data[i] = d[i];
    assert(data.size() == m_n);
    if (m_make_corrections) {
        // we want the right end of the codeword to have location 0 -- this makes the index for coefficients the same as their corresponding monomial power
        std::reverse(data.begin(), data.end());
        m_statistics["numberOfCalls"] += 1;

        doDecode(data);

        std::reverse(data.begin(), data.end());
        for (int i = 0; i < m_n; i++)
            d[i] = data[i];
    }
}

template<int irreducible_poly, int alpha>

void ReedSolomon<irreducible_poly, alpha>::doDecode(std::vector<ByteWithErasureFlag> data) {
    int number_of_erasures = std::count_if(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool { return b.erased; });
    auto syndromes = computeSyndromes(data);

    if (std::all_of(syndromes.cbegin(), syndromes.cend(), [](int s) -> bool { return s == 0; })) {
        if (number_of_erasures == 0) {
            m_statistics["input ok"] += 1;
        } else {
            uneraseAll(data);
            m_statistics[fmt::format("syndromes zero with {} erasures, unerased", number_of_erasures)]++;
        }
    } else {
        int determinant = m_num.add(m_num.multiply(syndromes[1], syndromes[1]), m_num.multiply(syndromes[0], syndromes[2]));
        int c = (determinant == 0) ? tryCorrectOneError(syndromes, data) : tryCorrectTwoErrors(syndromes, data);
        bool corrected = false;
        if (c)
            corrected = true;
        else if (number_of_erasures == 2)
            corrected = tryDecodeOneErrorTwoErasures(syndromes, data);
        else if (number_of_erasures > 2)
            corrected = tryDecodeErasures(syndromes, data);

        if (corrected) {
            auto s = computeSyndromes(data);
            int number_of_non_zero = std::count_if(s.cbegin(), s.cend(), [](int s) -> bool { return s != 0; });
            if (number_of_non_zero != 0)
                printf("Still %d non-zero syndromes after correction", number_of_non_zero);
            uneraseAll(data);
        } else
            eraseAll(data);
    }
}

#endif //MUSECPP_REEDSOLOMON_H
