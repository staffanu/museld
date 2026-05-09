// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_FIRFILTERSTAGE_H
#define AC3RF_DECODE_FIRFILTERSTAGE_H

#include <memory>
#include <vector>

// Generic FIR filter stage with optional SIMD acceleration.
//
// CAVEAT — SIMD-vs-scalar fractional shift: when use_simd is true, the
// constructor pads the reversed filter with trailing zeros so its length
// becomes a multiple of the SIMD chunk width (8 for AVX, 4 for NEON).  Those
// zero taps live at the *low-index* end of the stored (reversed) buffer,
// which corresponds to extra delay at the *front* of the original impulse
// response.  The result is that the SIMD path computes a fractionally-shifted
// convolution compared to the scalar path whenever the original filter
// length is not already a multiple of the chunk width.  In practice this
// does not break the decoders (the DPLLs / timing recovery absorb the
// sub-sample shift), but mixing SIMD and scalar paths in the same chain
// will produce different output sample positions.  For a half-band filter,
// prefer HalfBandFirFilterStage which has no padding and exact scalar/SIMD
// parity.
class FirFilterStage {
public:
    FirFilterStage(
        std::string name,
        std::string description,
        std::vector<float> filter,
        int decimation_factor,
        int input_buffer_size_without_overlap,
        int output_offset,
        std::vector<float> *output_buffer,
        bool use_simd);

    FirFilterStage(const FirFilterStage &) = delete;
    FirFilterStage &operator=(const FirFilterStage &) = delete;
    FirFilterStage(FirFilterStage &&) = delete;
    FirFilterStage &operator=(FirFilterStage &&) = delete;

    ~FirFilterStage();

    static bool simdSupported();

    [[nodiscard]] std::string name() const;
    [[nodiscard]] int filterSize() const;
    [[nodiscard]] int decimationFactor() const;
    [[nodiscard]] std::vector<float> *inputBuffer();
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] int inputSampleAlignment() const;

    void applyFilter(int n);
    void moveDataToFront(int n);

private:
    void firFilter(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    static void firFilterNormal(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    constexpr static int c_AVX_floats_per_chunk = 8;
    static void firFilterAvx(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    static constexpr int c_NEON_floats_per_chunk = 4;
    static void firFilterNeon(const float *input, size_t input_length,
        const float *filter, size_t filter_size, float *output, int decimation_factor);

    std::string m_name;
    std::string m_description;
    std::vector<float> m_filter;
    int m_decimation_factor;
    int m_input_buffer_size_without_overlap;
    int m_output_offset;
    bool m_use_simd;

    // The input buffer is owned by the object and deallocated in the destructor
    std::vector<float> *m_input_buffer;

    std::vector<float> *m_output_buffer;
};

#endif //AC3RF_DECODE_FIRFILTERSTAGE_H
