//
// Created by staffanu on 4/10/23.
//

#ifndef MUSECPP_FILTERUTIL_H
#define MUSECPP_FILTERUTIL_H

#include <cstdint>
#include <array>

class FilterUtil {
public:
    // output size is 3 * input_size / 2
    static void ConvertSampleRate2to3(size_t input_size, uint8_t const *input, uint8_t *output);

    // phase is 0 if even rows should have even columns computed, 1 if odd rows should have even columns computed
    template <size_t height, size_t width>
    static void FilterImageDiamond(int phase, uint8_t (&buffer)[height][width]);

    template<size_t filter_height, size_t filter_width, size_t height, size_t width>
    static void FilterImage(
            std::array<std::array<int8_t, filter_width>, filter_height> filter,
            const uint8_t (&in)[height][width],
            uint8_t (&out)[height][width],
            uint8_t value_offset, // the value of "zero" -- used so that pixels at the edges are not biased
            uint16_t multiplier);

    static constexpr int s_color_filter_height = 5;
    static constexpr int s_color_filter_width = 9;
    static std::array<std::array<int8_t, s_color_filter_width>, s_color_filter_height> s_color_filter;
private:
    static constexpr int s_N2to3 = 9; // total 2 * 9 + 1 = 19 coefficients
    static std::array<int8_t, FilterUtil::s_N2to3 * 2 + 1> s_filter_2to3;

    static constexpr int s_diamond_filter_height = 7;
    static constexpr int s_diamond_filter_width = 9;
    static std::array<std::array<int8_t, s_diamond_filter_width>, s_diamond_filter_height> s_diamond_filter;
};

template<size_t height, size_t width>
void FilterUtil::FilterImageDiamond(int phase, uint8_t (&buffer)[height][width]) {
    constexpr int H = s_diamond_filter_height / 2;
    constexpr int W = s_diamond_filter_width / 2;

    constexpr int W_odd =  (W - 1) / 2 * 2 + 1; // rounded down to odd value

    for (int row = 0; row < height; row++)
        for (int col = (phase + row) % 2; col < width; col += 2) {
            int16_t s = 0;
            int i_start = -std::min(H, row);
            int i_max = std::min(H, (int)height - row - 1);
            for (int i = i_start; i <= i_max; i++) {
                int j_start = i % 2 == 0 ? -W_odd : -W;
                int j_max = std::min(W, (int)width - col - 1);
                for (int j = j_start; j <= j_max; j += 2)
                    if (col + j >= 0)
                        s += (int16_t)(buffer[row + i][col + j] * s_diamond_filter[i + H][j + W]);
            }
            buffer[row][col] = (uint8_t)(std::clamp(s * 2 / 256, 0, 255));
        }
}

template<size_t filter_height, size_t filter_width, size_t height, size_t width>
void FilterUtil::FilterImage(
        std::array<std::array<int8_t, filter_width>, filter_height> filter,
        const uint8_t (&in)[height][width],
        uint8_t (&out)[height][width],
        uint8_t value_offset,
        uint16_t multiplier) {
    constexpr int H = filter_height / 2;
    constexpr int W = filter_width / 2;

    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col ++) {
            int16_t s = 0;
            int i_start = -std::min(H, row);
            int i_max = std::min(H, (int)height - row - 1);
            for (int i = i_start; i <= i_max; i++) {
                int j_start = -std::min(W, col);
                int j_max = std::min(W, (int)width - col - 1);
                for (int j = j_start; j <= j_max; j ++)
                    s += (int16_t)((in[row + i][col + j] - value_offset) * filter[i + H][j + W]);
            }
            out[row][col] = (uint8_t)(std::clamp(s * multiplier / 256 + value_offset, 0, 255));
        }
}

#endif //MUSECPP_FILTERUTIL_H
