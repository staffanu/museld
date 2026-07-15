// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <format>
#include "ComplexFirFilterStage.h"

ComplexFirFilterStage::ComplexFirFilterStage(
    std::string name,
    std::string description,
    std::vector<float> filter,
    int decimation_factor,
    int input_buffer_size_without_overlap,
    int output_offset,
    std::vector<float> *output_re_buffer,
    std::vector<float> *output_im_buffer,
    bool use_simd)
  : m_name(name),
    m_description(description),
    m_re_filter(name, description, filter, decimation_factor, input_buffer_size_without_overlap, output_offset, output_re_buffer, use_simd),
    m_im_filter(name, description, filter, decimation_factor, input_buffer_size_without_overlap, output_offset, output_im_buffer, use_simd) {
}

ComplexFirFilterStage::~ComplexFirFilterStage() {
}

std::string ComplexFirFilterStage::name() const {
    return m_name;
}

int ComplexFirFilterStage::filterSize() const {
    return m_re_filter.filterSize();
}

int ComplexFirFilterStage::decimationFactor() const {
    return m_re_filter.decimationFactor();
}

std::vector<float> *ComplexFirFilterStage::inputReBuffer() {
    return m_re_filter.inputBuffer();
}
std::vector<float> *ComplexFirFilterStage::inputImBuffer() {
    return m_im_filter.inputBuffer();
}

std::string ComplexFirFilterStage::toString() const {
    return m_re_filter.toString();
}

int ComplexFirFilterStage::inputSampleAlignment() const {
    return m_re_filter.inputSampleAlignment();
}

void ComplexFirFilterStage::applyFilter(int n) {
    m_re_filter.applyFilter(n);
    m_im_filter.applyFilter(n);
}

void ComplexFirFilterStage::moveDataToFront(int n) {
    m_re_filter.moveDataToFront(n);
    m_im_filter.moveDataToFront(n);
}
