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

class ByteWithErasureFlag {
public:
    ByteWithErasureFlag() : m_value(0), m_erased(false) {}
    explicit ByteWithErasureFlag(int v) : m_value(v), m_erased(false) {}
    template<int irreducible_poly, int alpha> explicit ByteWithErasureFlag(GFValue<8, irreducible_poly, alpha> v) : m_value(v.getInt()), m_erased(false) {}
    ByteWithErasureFlag(int v, bool e) : m_value(v), m_erased(e) {}
    template<int irreducible_poly, int alpha> GFValue<8, irreducible_poly, alpha> gfValue() const {
        return GFValue<8, irreducible_poly, alpha>(m_value);
    }
    [[nodiscard]] uint8_t byteValue() const { return m_value; }
    [[nodiscard]] bool isErased() const { return m_erased; }
    void setErased(bool e) { m_erased = e; }
private:
    uint8_t m_value;
    bool m_erased;
};

template<int irreducible_poly, int alpha_decimal> class ReedSolomon {
public:
    // Notice this is only tested for k = n - 4.
    ReedSolomon(int n, int k, int fcr, bool make_corrections)
            : m_alpha(GFValue<8, irreducible_poly, alpha_decimal>(alpha_decimal)),
              m_n(n),
              m_k(k),
              m_fcr(fcr),
              m_make_corrections(make_corrections),
              m_unit(GFValue<8, irreducible_poly, alpha_decimal>(1)),
              m_alpha_inverse(m_alpha.inverse()),
              m_alpha_squared(m_alpha * m_alpha) {
        for (int row = 0; row < n - k; row++) {
            std::vector<GFValue<8, irreducible_poly, alpha_decimal>> h_row;
            for (int col = 0; col < n; col++) {
                h_row.push_back(GFValue<8, irreducible_poly, alpha_decimal>::alpha_pow((row + fcr) * col));
            }
            H.push_back(h_row);
        }
    }

    // The first symbol in the input is the highest order coefficient
    void decode(std::vector<ByteWithErasureFlag> &data);

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
    GFValue<8, irreducible_poly, alpha_decimal> m_alpha;
    int m_n;
    int m_k;
    int m_fcr;
    bool m_make_corrections;
    GFValue<8, irreducible_poly, alpha_decimal> m_unit;
    GFValue<8, irreducible_poly, alpha_decimal> m_alpha_inverse;
    GFValue<8, irreducible_poly, alpha_decimal> m_alpha_squared;

    std::vector<std::vector<GFValue<8, irreducible_poly, alpha_decimal>>> H;
    std::map<std::string, int> m_statistics;

    void doDecode(std::vector<ByteWithErasureFlag> &data);

    std::vector<GFValue<8, irreducible_poly, alpha_decimal>> computeSyndromes(std::vector<ByteWithErasureFlag> const &data) {
        std::vector<GFValue<8, irreducible_poly, alpha_decimal>> syndromes;
        for (int i = 0; i < m_n - m_k; i++) {
            GFValue<8, irreducible_poly, alpha_decimal> s = GFValue<8, irreducible_poly, alpha_decimal>(0);
            for (int j = 0; j < m_n; j++)
                s = s + H[i][j] * data[j].gfValue<irreducible_poly, alpha_decimal>();
            syndromes.push_back(s);
        }
        return syndromes;
    }

    void eraseAll(std::vector<ByteWithErasureFlag> const &data) {
        for (auto b: data)
            b.setErased(true);
    }

    void uneraseAll(std::vector<ByteWithErasureFlag> &data) {
        for (auto b: data)
            b.setErased(false);
        m_statistics["unerased all"]++;
    }

    bool tryCorrectOneError(std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &syndromes, std::vector<ByteWithErasureFlag> &data) {
            if (std::count_if(syndromes.cbegin(), syndromes.cend(), [](GFValue<8, irreducible_poly, alpha_decimal> a) -> bool { return a.isZero(); })) {
                m_statistics["single error some syndrome zero"]++;
                return false;
            } else {
                auto x0 = syndromes[1] * syndromes[0].inverse();
                int errorPos0 = x0.log();
                auto y0 = syndromes[0] * m_alpha.pow(errorPos0 * m_fcr).inverse();

                if (errorPos0 < 0 || errorPos0 >= m_n) {
                    m_statistics["single error outside range"] += 1;
                    return false;
                } else if (syndromes[2] * syndromes[1].inverse() != x0 ||
                           syndromes[3] * syndromes[2].inverse() != x0) {
                    m_statistics["single error inconsistent syndromes"] += 1;
                    return false;
                } else {
                    data[errorPos0] = ByteWithErasureFlag(data[errorPos0].gfValue<irreducible_poly, alpha_decimal>() + y0);
                    m_statistics["single correction count"] += 1;
                    return true;
                }
            }
    }

    bool tryCorrectTwoErrors(std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &syndromes, std::vector<ByteWithErasureFlag> &data) {
            auto determinant = syndromes[1] * syndromes[1] + syndromes[0] * syndromes[2];
            if (determinant.isZero()) {
                m_statistics["dual determinant zero"];
                return false;
            } else {
                auto detInv = determinant.inverse();
                auto lambda1 = (syndromes[1] * syndromes[2] + syndromes[0] * syndromes[3]) * detInv;
                auto lambda2 = (syndromes[2] * syndromes[2] + syndromes[1] * syndromes[3]) * detInv;

                std::vector<std::pair<GFValue<8, irreducible_poly, alpha_decimal>, int>> roots;
                auto alphaPowI = m_unit;
                auto lambda1TimesAlphaPowI = lambda1; // for i == 0
                auto lambda2TimesAlphaPow2I = m_unit; // we swap lambda0 (which is 1) and lambda2 in order to get the error locations directly without inversion
                for (int i = 0; i < m_n; i++) {
                    if (lambda1TimesAlphaPowI + lambda2TimesAlphaPow2I == lambda2)
                        roots.emplace_back(alphaPowI, i);
                    alphaPowI = alphaPowI * m_alpha;
                    lambda1TimesAlphaPowI = lambda1TimesAlphaPowI * m_alpha;
                    lambda2TimesAlphaPow2I = lambda2TimesAlphaPow2I * m_alpha_squared;
//        println(f"$i: ${alphaPowI.a}%02x ${alphaPowMinusI.a}%02x ${lambda1TimesAlphaPowI.a}%02x ${lambda2TimesAlphaPow2I.a}%02x")
                }

                if (roots.size() != 2) {
                    m_statistics["dual too few roots"] += 1;
                    return false;
                } else {
                    auto [x0, errorPos0] = roots[0];
                    auto [x1, errorPos1] = roots[1];
                    assert(errorPos0 >= 0 && errorPos0 < m_n && errorPos1 >= 0 && errorPos1 < m_n);

                    auto x0PlusX1Inv = (x0 + x1).inverse();
                    auto y0 = (syndromes[1] + syndromes[0] * x1) * x0PlusX1Inv * m_alpha.pow(errorPos0 * m_fcr).inverse();
                    auto y1 = (syndromes[1] + syndromes[0] * x0) * x0PlusX1Inv * m_alpha.pow(errorPos1 * m_fcr).inverse();

                    ByteWithErasureFlag data0 = data[errorPos0];
                    ByteWithErasureFlag data1 = data[errorPos1];
                    data[errorPos0] = ByteWithErasureFlag(data0.gfValue<irreducible_poly, alpha_decimal>() + y0);
                    data[errorPos1] = ByteWithErasureFlag(data1.gfValue<irreducible_poly, alpha_decimal>() + y1);

                    m_statistics["dual correction count"] += 1;
                    return true;
                }
                m_statistics["dual correction not implemented"] += 1;
                return false;
            }
    }

    bool tryDecodeErasures(std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &syndromes, std::vector<ByteWithErasureFlag> &data) {
        std::vector<int> errorPositions;
        for (int i = 0; i < m_n; i++)
            if (data[i].isErased())
                errorPositions.push_back(i);
        if (errorPositions.size() > syndromes.size()) {
            m_statistics["erasure decoding failed with ${errorPositions.size} erasures"]++;
            return false;
        } else {
            std::vector<GFValue<8, irreducible_poly, alpha_decimal>> x(errorPositions.size());
            std::transform(errorPositions.begin(), errorPositions.end(), x.begin(), [this](int b) -> GFValue<8, irreducible_poly, alpha_decimal> { return m_alpha.pow(b); });
            auto s = syndromes;
            s.resize(errorPositions.size());
            std::vector<GFValue<8, irreducible_poly, alpha_decimal>> y = solveSyndromeEquations(x, s);

            for (int i = 0; i < errorPositions.size(); i++) {
                int errorPos = errorPositions[i];
                auto error = y[i];
                data[errorPos] = ByteWithErasureFlag(data[errorPos].gfValue<irreducible_poly, alpha_decimal>() + error * m_alpha.pow(errorPos * m_fcr).inverse());
            }
            m_statistics[fmt::format("corrected {} erasures", errorPositions.size())]++;
            return true;
        }
    }

    bool tryDecodeOneErrorTwoErasures(std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &syndromes, std::vector<ByteWithErasureFlag> &data) {
        std::vector<int> errorPositions;
        for (int i = 0; i < m_n; i++)
            if (data[i].isErased())
                errorPositions.push_back(i);
        if (errorPositions.size() != 2) {
            m_statistics["one error two erasures needs two erasures"]++;
            return false;
        } else {
            // See "Method for correcting both errors and erasures of RS codes using error-only and erasure-only decoding algorithms"
            // by Erl-Huei Lu, Pen-Yao Lu, Tso-Cho Chen (https://doi.org/10.1049/el.2013.1521)

            // Think of y0 and y1 being the erased errors at locators x0 and x1
            auto alphaInv = m_alpha_inverse;
            auto s0prime = syndromes[0] + syndromes[1] * alphaInv.pow(errorPositions[0]);
            auto s1prime = syndromes[1] * alphaInv.pow(errorPositions[0]) +
                          syndromes[2] * alphaInv.pow(2 * errorPositions[0]);
            auto s2prime = syndromes[2] * alphaInv.pow(2 * errorPositions[0]) +
                          syndromes[3] * alphaInv.pow(3 * errorPositions[0]);
            int posDiff = errorPositions[1] - errorPositions[0]; // positive
            auto s0bis = s0prime + s1prime * alphaInv.pow(posDiff);
            auto s1bis = s1prime * alphaInv.pow(posDiff) + s2prime * alphaInv.pow(2 * posDiff);

            auto x2prime = s1bis * s0bis.inverse();
            int errorPos2prime = x2prime.log(); // this is errorPos2 - errorPos1
            int errorPos2 = (errorPos2prime + errorPositions[1]) % 255;
            if (errorPos2 >=0  && errorPos2 < m_n) {
                auto y2prime = s0bis;
                auto x0 = GFValue<8, irreducible_poly, alpha_decimal>::alpha_pow(errorPositions[0]);
                auto x2 = GFValue<8, irreducible_poly, alpha_decimal>::alpha_pow(errorPos2);
                auto y2BeforeFcrAdjust =
                        y2prime * (m_unit + x0.inverse() * x2).inverse() * (m_unit + x2prime).inverse();
                auto y2 = y2BeforeFcrAdjust *
                          GFValue<8, irreducible_poly, alpha_decimal>::alpha_pow(errorPos2 * m_fcr).inverse();

                data[errorPos2] = ByteWithErasureFlag(data[errorPos2].gfValue<irreducible_poly, alpha_decimal>() + y2);

                auto fixedSyndromes = std::vector<GFValue<8, irreducible_poly, alpha_decimal>>{
                        syndromes[0] + y2BeforeFcrAdjust,
                        syndromes[1] +
                        y2BeforeFcrAdjust * GFValue<8, irreducible_poly, alpha_decimal>::alpha_pow(errorPos2)
                };

                bool c = tryDecodeErasures(fixedSyndromes, data);
                if (c)
                    m_statistics["single correction with two erasures"]++;
                else
                    m_statistics["single correction with two erasures failed on erasures"]++;
                return c;
            } else {
                m_statistics["single correction with two erasures failed errorPos2 out of range"]++;
                return false;
            }
        }
    }

    /**
     * Solves the equation system
     *
     * x(0)^k y(0) + ... + x(n-1)^k y(n-1) = s(k), for all k = 0...n-1
     */
    std::vector<GFValue<8, irreducible_poly, alpha_decimal>>
    solveSyndromeEquations(std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &x, std::vector<GFValue<8, irreducible_poly, alpha_decimal>> const &s) {
        int m = (int)x.size();
        assert(s.size() == m);
        assert(std::count_if(x.cbegin(), x.cend(), [](GFValue<8, irreducible_poly, alpha_decimal> a) -> bool { return a.isZero(); }) == 0);

        // compute the matrix by multiplying x element wise to the previous rows
        std::vector<GFValue<8, irreducible_poly, alpha_decimal>> row;
        row.resize(m);
        std::fill(row.begin(), row.end(), GFValue<8, irreducible_poly, alpha_decimal>(1));

        std::vector<std::vector<GFValue<8, irreducible_poly, alpha_decimal>>> mat;
        mat.resize(m);
        for (int i = 0; i < m; i++) {
            mat[i] = row;
            for (int j = 0; j < m; j++)
                row[j] *= x[j];
        }

        auto y = s;
        for (int i = 0; i < m - 1; i++) { // for each row, clear column i under it
            auto leadingInverse = mat[i][i].inverse();
            for (int j = i; j < m; j++) // make first element unit
                mat[i][j] = mat[i][j] * leadingInverse;
            y[i] *= leadingInverse;

            for (int k = i + 1; k < m; k++) { // rows under
                auto leading = mat[k][i];
                for (int j = i; j < m; j++)
                    mat[k][j] = mat[k][j] + leading * mat[i][j];
                y[k] = y[k] + leading * y[i];
            }
        }

        // make bottom right element unit
        auto brInverse = mat[m - 1][m - 1].inverse();
        mat[m - 1][m - 1] = mat[m - 1][m - 1] * brInverse;
        y[m - 1] *= brInverse;

        for (int i = m - 1; i > 0; i--) { // for each row, clear column i over it
            for (int k = i - 1; k >= 0; k--) { // rows over
                y[k] += mat[k][i] * y[i];
            }
        }

        return y;
    }
};

template<int irreducible_poly, int alpha>
void ReedSolomon<irreducible_poly, alpha>::decode(std::vector<ByteWithErasureFlag> &data) {
    assert(data.size() == m_n);
    if (m_make_corrections) {
        // we want the right end of the codeword to have location 0 -- this makes the index for coefficients the same as their corresponding monomial power
        std::reverse(data.begin(), data.end());
        m_statistics["numberOfCalls"] += 1;

        doDecode(data);

        std::reverse(data.begin(), data.end());
    }
}

template<int irreducible_poly, int alpha>
void ReedSolomon<irreducible_poly, alpha>::doDecode(std::vector<ByteWithErasureFlag> &data) {
    int number_of_erasures = std::count_if(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool { return b.isErased(); });
    auto syndromes = computeSyndromes(data);

    if (std::all_of(syndromes.cbegin(), syndromes.cend(), [](GFValue<8, irreducible_poly, alpha> s) -> bool { return s.isZero(); })) {
        if (number_of_erasures == 0) {
            m_statistics["input ok"] += 1;
        } else {
            uneraseAll(data);
            m_statistics[fmt::format("syndromes zero with {} erasures, un-erased", number_of_erasures)]++;
        }
    } else {
        auto determinant = syndromes[1] * syndromes[1] + syndromes[0] * syndromes[2];

        bool corrected = (determinant.isZero()) ? tryCorrectOneError(syndromes, data) : tryCorrectTwoErrors(syndromes, data);
        if (!corrected) {
            if (number_of_erasures == 2)
                corrected = tryDecodeOneErrorTwoErasures(syndromes, data);
            else if (number_of_erasures > 2)
                corrected = tryDecodeErasures(syndromes, data);
        }

        if (corrected) {
            auto s = computeSyndromes(data);
            int number_of_non_zero = std::count_if(s.cbegin(), s.cend(), [](GFValue<8, irreducible_poly, alpha> s) -> bool { return s.nonZero(); });
            if (number_of_non_zero != 0)
                printf("Still %d non-zero syndromes after correction", number_of_non_zero);
            uneraseAll(data);
        } else
            eraseAll(data);
    }
}

#endif //MUSECPP_REEDSOLOMON_H
