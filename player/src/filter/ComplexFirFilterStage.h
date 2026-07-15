// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef AC3RF_DECODE_COMPLEXFIRFILTERSTAGE_H
#define AC3RF_DECODE_COMPLEXFIRFILTERSTAGE_H

#include <memory>
#include <vector>

#include "FirFilterStage.h"

class ComplexFirFilterStage {
public:
    ComplexFirFilterStage(
        std::string name,
        std::string description,
        std::vector<float> filter,
        int decimation_factor,
        int input_buffer_size_without_overlap,
        int output_offset,
        std::vector<float> *output_re_buffer,
        std::vector<float> *output_im_buffer,
        bool use_simd);

    ComplexFirFilterStage(const ComplexFirFilterStage &) = delete;
    ComplexFirFilterStage &operator=(const ComplexFirFilterStage &) = delete;
    ComplexFirFilterStage(ComplexFirFilterStage &&) = delete;
    ComplexFirFilterStage &operator=(ComplexFirFilterStage &&) = delete;

    ~ComplexFirFilterStage();

    [[nodiscard]] std::string name() const;
    [[nodiscard]] int filterSize() const;
    [[nodiscard]] int decimationFactor() const;
    [[nodiscard]] std::vector<float> *inputReBuffer();
    [[nodiscard]] std::vector<float> *inputImBuffer();
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] int inputSampleAlignment() const;

    void applyFilter(int n);
    void moveDataToFront(int n);

private:
    std::string m_name;
    std::string m_description;
    FirFilterStage m_re_filter;
    FirFilterStage m_im_filter;
};

#endif //AC3RF_DECODE_COMPLEXFIRFILTERSTAGE_H
