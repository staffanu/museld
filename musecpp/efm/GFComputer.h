//
// Created by Staffan Ulfberg on 2/8/24.
//

#ifndef MUSECPP_GFCOMPUTER_H
#define MUSECPP_GFCOMPUTER_H

#include <cassert>
#include <array>
#include <vector>
#include <stdexcept>

namespace GfComputerHelper {
    template<int bits, int irreducible_poly> int multiply_impl(int a, int b) {
        int aa = a;
        int bb = b;
        int res = 0;
        while (bb != 0) {
            if ((bb & 1) != 0)
                res ^= aa;
            if ((aa & (1 << (bits - 1))) != 0)
                aa = (aa << 1) ^ irreducible_poly;
            else aa <<= 1;
            bb >>= 1;
        }
        return res;
    }

    template<int bits, int irreducible_poly>
    std::array<int, 1 << bits> make_inverse_table() {
        std::array<int, 1 << bits> table;
        table[0] = -1; // error
        for (int i = 1; i < (1 << bits); i++) {
            for (int j = 1; j < (1 << bits); j++)
                if (multiply_impl<bits, irreducible_poly>(i, j) == 1)
                    table[i] = j;
        }
        return table;
    }

    template<int bits, int irreducible_poly>
    std::array<int, 1 << bits> make_log_table(int alpha) {
        std::array<int, 1 << bits> table;
        table[0] = -1;
        int v = 1;
        for (int i = 1; i <= ( 1 << bits) - 2; i++) {
            v = multiply_impl<bits, irreducible_poly>(v, alpha);
            table[v] = i;
        }
        return table;
    }

    template<int bits, int irreducible_poly>
    std::array<int, 1 << bits> make_pow_table(int alpha) {
        std::array<int, 1 << bits> table;
        int v = 1;
        for (int i = 0; i < ( 1 << bits); i++) {
            table[i] = v;
            v = multiply_impl<bits, irreducible_poly>(v, alpha);
        }
        return table;
    }
}

using namespace GfComputerHelper;

// Makes computations in GF[2^bits] mod the given irreducible polynomial
template <int bits, int irreducible_poly, int alpha> class GFComputer {
public:
    static int add(int a, int b) {
        return a ^ b;
    }

    static int multiply(int a, int b) {
        return multiply_impl<bits, irreducible_poly>(a, b);
    }

    static int inverse(int a) {
        assert(a != 0);
        return inverse_table[a];
    }
    static int log(int a) {
        assert(a != 0);
        return log_table[a];
    }
    static int alpha_pow(int i) {
        return pow_table[i % ((1 << bits) - 1)];
    }

    static int pow(int a, int b) {
        return alpha_pow(b * log(a));
    }

private:
    static const std::array<int, 1 << bits> inverse_table;
    static const std::array<int, 1 << bits> log_table;
    static const std::array<int, 1 << bits> pow_table;
};

template <int bits, int irreducible_poly, int alpha>
const std::array<int, 1 << bits> GFComputer<bits, irreducible_poly, alpha>::inverse_table = make_inverse_table<bits, irreducible_poly>();

template <int bits, int irreducible_poly, int alpha>
const std::array<int, 1 << bits> GFComputer<bits, irreducible_poly, alpha>::log_table = make_log_table<bits, irreducible_poly>(alpha);

template <int bits, int irreducible_poly, int alpha>
const std::array<int, 1 << bits> GFComputer<bits, irreducible_poly, alpha>::pow_table = make_pow_table<bits, irreducible_poly>(alpha);

#endif //MUSECPP_GFCOMPUTER_H
