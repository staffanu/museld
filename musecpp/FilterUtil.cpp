//
// Created by staffanu on 4/10/23.
//

#include <numeric>
#include <cassert>
#include "MuseTypes.h"
#include "FilterUtil.h"

using namespace std;

array<int8_t, FilterUtil::s_N2to3 * 2 + 1> FilterUtil::s_filter_2to3 = []() constexpr {
    array<double, FilterUtil::s_N2to3 * 2 + 1> arr = {
            // cutoff 0.25, transition 0.05, sampling freq 1, Rectangular: 19 coeffs (25 non-zero).  Looks +/- 9 samples in each direction
            0.034286805542972851, -0.000000000000000019, -0.044083035698107946, 0.000000000000000019, 0.061716249977351118,-0.000000000000000019, -0.102860416628918538,
            0.000000000000000019, 0.308581249886755615, 0.484718293839893788, 0.308581249886755615, 0.000000000000000019, -0.102860416628918538, -0.000000000000000019,
            0.061716249977351118, 0.000000000000000019, -0.044083035698107946, -0.000000000000000019, 0.034286805542972851
    };
    assert(fabs(accumulate(arr.cbegin(), arr.cend(), 0.0) - 1.0) < 1e-10);
    assert(all_of(arr.cbegin(), arr.cend(), [](double d) -> bool { return fabs(d) < 0.5; }));

    array<int8_t, FilterUtil::s_N2to3 * 2 + 1> filter{};
    for (int i = 0; i < arr.size(); i++)
        filter[i] = (int8_t)(arr[i] * 256);
    return filter;
}();

void FilterUtil::ConvertSampleRate2to3(size_t input_size, uint8_t const *input, uint8_t *output) {
    assert(input_size % 2 == 0);
    // Convert to 3/2 of the input sample rate by inserting two zeros between samples to up-sample 3x,
    // low-pass filter at 0.5 times the new Nyquist rate and then select every 2nd sample

    // We do not represent the up-sampled signal explicitly, and also do not represent the entire filter output
    // since we only need every 2nd sample

    for (int hiFreqIx = 0; hiFreqIx < input_size * 3; hiFreqIx +=2) {
        //cout << "Output index: " << i << ": ";
        int32_t s = 0;
        for (int l = (hiFreqIx - s_N2to3 + 2) / 3; l <= (hiFreqIx + s_N2to3) / 3; l++) {
            uint8_t v = l >= 0 && l < input_size ? input[l] : 128;
            s += v * s_filter_2to3[3 * l - hiFreqIx + s_N2to3];
            //cout << "(" << l << "," << (3 * l - hiFreqIx + s_N2to3) << ")";
        }
        //cout << endl;
        output[hiFreqIx / 2] = (uint8_t)clamp(s * 3 / 256, 0, 255);
    }
}

array<array<int8_t, FilterUtil::s_diamond_filter_width>, FilterUtil::s_diamond_filter_height>
FilterUtil::s_diamond_filter = []() constexpr {
    array<array<double, FilterUtil::s_diamond_filter_width>, FilterUtil::s_diamond_filter_height> arr = {
            -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
            0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
            -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
            0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
            -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
            0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
            -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
    };

    double sum = accumulate(arr.cbegin(), arr.cend(), 0.0, [](double lhs, const auto &rhs) {
        return accumulate(rhs.cbegin(), rhs.cend(), lhs);
    });
    assert(fabs(sum - 1.0) < 1e-5);

    array<array<int8_t, FilterUtil::s_diamond_filter_width>, FilterUtil::s_diamond_filter_height> filter{};
    for (int i = 0; i < FilterUtil::s_diamond_filter_height; i++)
        for (int j = 0; j < FilterUtil::s_diamond_filter_width; j++)
            filter[i][j] = (int8_t)(arr[i][j] * 256);

    return filter;
}();
