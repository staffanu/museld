//
// Created by staffanu on 5/6/23.
//

#include <stdexcept>
#include <fstream>
#include <iostream>
#include "MuseTypes.h"
#include "MuseBuffer.h"
#include "Shaders.h"

using namespace std;

Shaders *Shaders::s_singleton = nullptr;

void Shaders::CreateInstance() {
    s_singleton = new Shaders();
};

Shaders &Shaders::GetInstance() {
    return *s_singleton;
}

kp::Manager &Shaders::GetManager() {
    return s_singleton->m_mgr;
}

std::vector<uint32_t> Shaders::CompileSource(const std::string &filename) {
    auto command = string("glslangValidator -V " + filename + " -o " + filename + ".spv");
    cout << "command: " << command << endl;
    if (system(command.c_str()))
        throw std::runtime_error("Error running glslangValidator command");
    ifstream fileStream(filename + ".spv", ios::binary);
    vector<char> buffer;
    buffer.insert(buffer.begin(), istreambuf_iterator<char>(fileStream), {});
    return {(uint32_t*)buffer.data(), (uint32_t*)(buffer.data() + buffer.size())};
}

Shaders::Shaders()
: m_mgr(kp::Manager(0, {}, { "VK_EXT_shader_atomic_float" })) {
    m_diamond_spirv = CompileSource("../../musecpp/shaders/filter_diamond.comp");
    m_filter_image_spirv = CompileSource("../../musecpp/shaders/filter_image.comp");
    m_apply_transmission_gamma_y_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_y.comp");
    m_apply_transmission_gamma_c_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_c.comp");
    m_convert_2_to_3_spirv = CompileSource("../../musecpp/shaders/convert_horiz_sample_rate_2_to_3.comp");
    m_fill_empty_lines_spirv = CompileSource("../../musecpp/shaders/fill_empty_lines.comp");
    m_combine_y_and_rb_spirv = CompileSource("../../musecpp/shaders/combine_y_and_rb.comp");
    m_decode_c_spirv = CompileSource("../../musecpp/shaders/decode_c.comp");

    m_diamond_filter_tensor = m_mgr.tensor(
            {
                    -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
                    0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                    -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                    0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
                    -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                    0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                    -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
            });

    m_color_filter_tensor = m_mgr.tensor(
            {
                    0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
                    0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                    0.008117, 0.021787, 0.054789, 0.087791, 0.101461, 0.087791, 0.054789, 0.021787, 0.008117,
                    0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                    0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
            });

    m_filter_2_to_3_tensor = m_mgr.tensor(
            {
                    // cutoff 0.25, transition 0.05, sampling freq 1, Rectangular: 19 coeffs (25 non-zero).  Looks +/- 9 samples in each direction
                    0.034286805542972851, -0.000000000000000019, -0.044083035698107946, 0.000000000000000019, 0.061716249977351118,-0.000000000000000019, -0.102860416628918538,
                    0.000000000000000019, 0.308581249886755615, 0.484718293839893788, 0.308581249886755615, 0.000000000000000019, -0.102860416628918538, -0.000000000000000019,
                    0.061716249977351118, 0.000000000000000019, -0.044083035698107946, -0.000000000000000019, 0.034286805542972851
            });
}

void Shaders::ApplyTransmissionGamma(MuseBuffer<float> &buffer) {
    std::shared_ptr<kp::Algorithm> algo_y =
            m_mgr.algorithm({buffer.tensor()},
                            m_apply_transmission_gamma_y_spirv,
                            kp::Workgroup({MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {});
    std::shared_ptr<kp::Algorithm> algo_c =
            m_mgr.algorithm({buffer.tensor()},
                            m_apply_transmission_gamma_c_spirv,
                            kp::Workgroup({MUSE_C_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({m_diamond_filter_tensor, buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(algo_y)
            ->record<kp::OpAlgoDispatch>(algo_c)
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::FilterImageDiamond(int phase, MuseBuffer<float> &buffer) {
    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({m_diamond_filter_tensor, buffer.tensor()},
                            m_diamond_spirv, kp::Workgroup({buffer.width(), buffer.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({m_diamond_filter_tensor, buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{m_diamond_filter_height, m_diamond_filter_width, buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::FilterImage(unsigned int filter_height, unsigned int filter_width, std::shared_ptr<kp::Tensor> &filter,
                          MuseBuffer<float> &source, MuseBuffer<float> &dest,
                          float border_value, float multiplier) {
    assert(source.height() == dest.height());
    assert(source.width() == dest.width());

    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({filter, source.tensor(), dest.tensor()},
                            m_filter_image_spirv, kp::Workgroup({source.width(), source.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({filter, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{(float)filter_height, (float)filter_width, (float)source.height(), (float)source.width(), border_value, multiplier})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()})
            ->eval();
}

void Shaders::ConvertHorizSampleRate2to3(MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                         unsigned int output_start_row, unsigned int output_row_step_size) {
    assert(source.width() * 3 == dest.width() * 2);
    assert(source.height() * output_row_step_size == dest.height());

    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({m_filter_2_to_3_tensor, source.tensor(), dest.tensor()},
                            m_convert_2_to_3_spirv, kp::Workgroup({dest.width(), dest.height() / output_row_step_size}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({m_filter_2_to_3_tensor, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{m_filter_2_to_3_tensor->size(), source.height(), source.width(), output_start_row, output_row_step_size})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()})
            ->eval();
}

void Shaders::FillEmptyLines(MuseBuffer<float> &buffer, int phase) {
    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({buffer.tensor()},
                            m_fill_empty_lines_spirv, kp::Workgroup({buffer.width(), buffer.height() / 2}),
                            {}, {0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::CombineYandRB(MuseBuffer<float> &Y_data, MuseBuffer<float> &C_r_data,
                            MuseBuffer<float> &C_b_data, MuseBuffer<uint32_t> &out_rgb) {
    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor(), out_rgb.tensor()},
                            m_combine_y_and_rb_spirv, kp::Workgroup({Y_data.width(), Y_data.height()}),
                            {}, {0, 0, 0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{Y_data.height(), Y_data.width(), C_r_data.width()})
            ->record<kp::OpTensorSyncLocal>({out_rgb.tensor()})
            ->eval();
}

void Shaders::DecodeC(MuseBuffer<float> &input_frame, MuseBuffer<float> &C_r_data,
                      MuseBuffer<float> &C_b_data, int frame_phase_c, int field_parity) {
    std::shared_ptr<kp::Algorithm> algo =
            m_mgr.algorithm({input_frame.tensor(), C_r_data.tensor(), C_b_data.tensor()},
                            m_decode_c_spirv, kp::Workgroup({C_r_data.width(), C_r_data.height()}),
                            {}, {0, 0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({input_frame.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    algo,
                    std::vector{frame_phase_c, field_parity})
            ->record<kp::OpTensorSyncLocal>({C_r_data.tensor(), C_b_data.tensor()})
            ->eval();
}
