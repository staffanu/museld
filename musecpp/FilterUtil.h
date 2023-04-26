//
// Created by staffanu on 4/10/23.
//

#ifndef MUSECPP_FILTERUTIL_H
#define MUSECPP_FILTERUTIL_H


#include <cstdint>
#include <cstddef>
#include <array>

class FilterUtil {
public:
    // output size is 3 * input_size / 2
    static void ConvertSampleRate2to3(size_t input_size, DecoderInt const *input, DecoderInt *output);

    template <size_t height, size_t width>
    static void FilterImageDiamond(DecoderInt const (&input)[height][width], DecoderInt (&output)[height][width]);

private:
    static const int s_N2to3 = 9; // total 2 * 9 + 1 = 19 coefficients
    static std::array<double, FilterUtil::s_N2to3 * 2 + 1> s_filter_2to3;

    static const int s_diamond_filter_height = 7;
    static const int s_diamond_filter_width = 9;
    static std::array<std::array<double, s_diamond_filter_width>, s_diamond_filter_height> s_diamond_filter;
};

template<size_t height, size_t width>
void FilterUtil::FilterImageDiamond(DecoderInt const (&input)[height][width], DecoderInt (&output)[height][width]) {
    constexpr int H = s_diamond_filter_height / 2;
    constexpr int W = s_diamond_filter_width / 2;

    double filter[s_diamond_filter_height][s_diamond_filter_width];
    for (int i = 0; i < s_diamond_filter_height; i++)
        for (int j = 0; j < s_diamond_filter_width; j++)
            filter[i][j] = s_diamond_filter[i][j];

    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++) {
            double s = 0;
            for (int i = -W; i <= W; i++)
                for (int j = -H; j <= H; j++)
                    if (row + i >= 0 && row + i < height && col + j >= 0 && col + j < width)
                        s += input[row + i][col + j] * filter[i + W][j + H];
            s = std::clamp(s, 0.0, 255.99 * MUSE_MULT);
            output[row][col] = (DecoderInt)(s * 2);
        }
}

#endif //MUSECPP_FILTERUTIL_H
