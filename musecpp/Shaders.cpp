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
: m_mgr(kp::Manager(0, {}, { "VK_EXT_shader_atomic_float" })),
  m_interpolated32_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_field_Y_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_C_r_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_C_b_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_r_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_frame_out_buffer(Shaders::CreateMuseBuffer<uint32_t>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_diamond_filter_buffer(MuseBuffer<float>(7, 9, m_mgr.tensor(
          {
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
          })))
{
    m_diamond_spirv = CompileSource("../../musecpp/shaders/filter_diamond.comp");
    m_filter_image_spirv = CompileSource("../../musecpp/shaders/filter_image.comp");
    m_apply_transmission_gamma_y_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_y.comp");
    m_apply_transmission_gamma_c_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_c.comp");
    m_copy_y_for_interpolation_spirv = CompileSource("../../musecpp/shaders/copy_y_for_interpolation.comp");
    m_convert_2_to_3_spirv = CompileSource("../../musecpp/shaders/convert_horiz_sample_rate_2_to_3.comp");
    m_fill_empty_lines_spirv = CompileSource("../../musecpp/shaders/fill_empty_lines.comp");
    m_combine_y_and_rb_spirv = CompileSource("../../musecpp/shaders/combine_y_and_rb.comp");
    m_decode_c_spirv = CompileSource("../../musecpp/shaders/decode_c.comp");

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

template<typename T>
MuseBuffer<T> Shaders::CreateMuseBuffer(unsigned int height, unsigned int width) {
    kp::Tensor::TensorDataTypes dataType;
    if (std::is_same<T, float>::value)
        dataType = kp::Tensor::TensorDataTypes::eFloat;
    else if (std::is_same<T, int32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eInt;
    else if (std::is_same<T, uint32_t>::value)
        dataType = kp::Tensor::TensorDataTypes::eUnsignedInt;
    else
        throw std::runtime_error("Unknown type T");

    // the initial data isn't used
    auto *data = new T[height * width];
    auto tensor = m_mgr.tensor(data, height * width, sizeof(T), dataType, kp::Tensor::TensorTypes::eDevice);
    auto buffer = MuseBuffer<T>(height, width, tensor);
    delete[] data;
    return buffer;
}

cv::Mat Shaders::DecodeSingleField(FieldBufferView &field) {
    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.m_control.has_value() ?
                        field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.m_control.has_value() ?
                        field.m_control.value().frame_subsampling_phase_C.value_or(0) : 0;
    // int field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);

//    CopyYForInterpolation(*field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y);
//    FilterImageDiamond(frame_phase_y, m_interpolated32_buffer);
//    ConvertHorizSampleRate2to3(m_interpolated32_buffer, m_field_Y_buffer, 1 - field_parity, 2);
//    FillEmptyLines(m_field_Y_buffer, field_parity);
//
//    DecodeC(*field.m_data, m_intermediate_r_buffer, m_intermediate_b_buffer,
//                    frame_phase_c, field_parity);
//    FilterImage(
//            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
//            m_color_filter_tensor, m_intermediate_r_buffer, m_field_C_r_buffer, 128.0 / 8, 8);
//    FilterImage(
//            Shaders::s_color_filter_height, Shaders::s_color_filter_width,
//            m_color_filter_tensor, m_intermediate_b_buffer, m_field_C_b_buffer, 128.0 / 8, 8);
//
//    CombineYandRB(m_field_Y_buffer, m_field_C_r_buffer, m_field_C_b_buffer, m_frame_out_buffer);

    std::shared_ptr<kp::Algorithm> copy_y_for_interpolation_algo =
            m_mgr.algorithm({field.tensor(), m_interpolated32_buffer.tensor()},
                            m_copy_y_for_interpolation_spirv, kp::Workgroup({MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {0, 0});
    std::shared_ptr<kp::Algorithm> diamond_algo =
            m_mgr.algorithm({m_diamond_filter_buffer.tensor(), m_interpolated32_buffer.tensor()},
                            m_diamond_spirv, kp::Workgroup({m_interpolated32_buffer.width(), m_interpolated32_buffer.height()}),
                            {}, {0, 0, 0, 0, 0});
    std::shared_ptr<kp::Algorithm> convert_2_to_3_algo =
            m_mgr.algorithm({m_filter_2_to_3_tensor, m_interpolated32_buffer.tensor(), m_field_Y_buffer.tensor()},
                            m_convert_2_to_3_spirv, kp::Workgroup({m_field_Y_buffer.width(), m_field_Y_buffer.height() / 2}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    std::shared_ptr<kp::Algorithm> fill_empty_lines_algo =
            m_mgr.algorithm({m_field_Y_buffer.tensor()},
                            m_fill_empty_lines_spirv, kp::Workgroup({m_field_Y_buffer.width(), m_field_Y_buffer.height() / 2}),
                            {}, {0.0, 0.0, 0.0});
    std::shared_ptr<kp::Algorithm> decode_c_algo =
            m_mgr.algorithm({field.tensor(), m_intermediate_r_buffer.tensor(), m_intermediate_b_buffer.tensor()},
                            m_decode_c_spirv, kp::Workgroup({m_intermediate_r_buffer.width(), m_intermediate_r_buffer.height()}),
                            {}, {0, 0});
    std::shared_ptr<kp::Algorithm> filter_color_r_algo =
            m_mgr.algorithm({m_color_filter_tensor, m_intermediate_r_buffer.tensor(), m_field_C_r_buffer.tensor()},
                            m_filter_image_spirv, kp::Workgroup({m_intermediate_r_buffer.width(), m_intermediate_r_buffer.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    std::shared_ptr<kp::Algorithm> filter_color_b_algo =
            m_mgr.algorithm({m_color_filter_tensor, m_intermediate_b_buffer.tensor(), m_field_C_b_buffer.tensor()},
                            m_filter_image_spirv, kp::Workgroup({m_intermediate_b_buffer.width(), m_intermediate_b_buffer.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    std::shared_ptr<kp::Algorithm> combine_y_and_rb_algo =
            m_mgr.algorithm({m_field_Y_buffer.tensor(), m_field_C_r_buffer.tensor(), m_field_C_b_buffer.tensor(), m_frame_out_buffer.tensor()},
                            m_combine_y_and_rb_spirv, kp::Workgroup({m_field_Y_buffer.width(), m_field_Y_buffer.height()}),
                            {}, {0, 0, 0});

    // There is really no reason to do SyncLocal / SyncDevice below -- they could be replaced with the appropriate
    // memory barriers.
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({field.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    copy_y_for_interpolation_algo,
                    std::vector{field_parity, frame_phase_y})
            ->record<kp::OpTensorSyncLocal>({m_interpolated32_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_diamond_filter_buffer.tensor(), m_interpolated32_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    diamond_algo,
                    std::vector{m_diamond_filter_buffer.height(), m_diamond_filter_buffer.width(), m_interpolated32_buffer.height(), m_interpolated32_buffer.width(), (unsigned)frame_phase_y})
            ->record<kp::OpTensorSyncLocal>({m_interpolated32_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_filter_2_to_3_tensor, m_interpolated32_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    convert_2_to_3_algo,
                    std::vector{m_filter_2_to_3_tensor->size(), m_interpolated32_buffer.height(), m_interpolated32_buffer.width(), 1u - field_parity, 2u})
            ->record<kp::OpTensorSyncLocal>({m_field_Y_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_field_Y_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    fill_empty_lines_algo,
                    std::vector{m_field_Y_buffer.height(), m_field_Y_buffer.width(), (unsigned)field_parity})
            ->record<kp::OpTensorSyncLocal>({m_field_Y_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({field.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    decode_c_algo,
                    std::vector{frame_phase_c, field_parity})
            ->record<kp::OpTensorSyncLocal>({m_intermediate_r_buffer.tensor(), m_intermediate_b_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_color_filter_tensor, m_intermediate_r_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    filter_color_r_algo,
                    std::vector{(float)Shaders::s_color_filter_height, (float)Shaders::s_color_filter_width, (float)m_intermediate_r_buffer.height(), (float)m_intermediate_r_buffer.width(), 128.0f / 8.0f, 8.0f})
            ->record<kp::OpTensorSyncLocal>({m_field_C_r_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_color_filter_tensor, m_intermediate_b_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    filter_color_b_algo,
                    std::vector{(float)Shaders::s_color_filter_height, (float)Shaders::s_color_filter_width, (float)m_intermediate_b_buffer.height(), (float)m_intermediate_b_buffer.width(), 128.0f / 8.0f, 8.0f})
            ->record<kp::OpTensorSyncLocal>({m_field_C_b_buffer.tensor()})

            ->record<kp::OpTensorSyncDevice>({m_field_Y_buffer.tensor(), m_field_C_r_buffer.tensor(), m_field_C_b_buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    combine_y_and_rb_algo,
                    std::vector{m_field_Y_buffer.height(), m_field_Y_buffer.width(), m_field_C_r_buffer.width()})
            ->record<kp::OpTensorSyncLocal>({m_frame_out_buffer.tensor()})
            ->eval();

    return { vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_8UC4, m_frame_out_buffer.data() };
}

void Shaders::ApplyTransmissionGamma(MuseBuffer<float> &buffer) {
    std::shared_ptr<kp::Algorithm> apply_transmission_gamma_y_algo =
            m_mgr.algorithm({buffer.tensor()},
                            m_apply_transmission_gamma_y_spirv,
                            kp::Workgroup({MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {});
    std::shared_ptr<kp::Algorithm> apply_transmission_gamma_c_algo =
            m_mgr.algorithm({buffer.tensor()},
                            m_apply_transmission_gamma_c_spirv,
                            kp::Workgroup({MUSE_C_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(apply_transmission_gamma_y_algo)
            ->record<kp::OpAlgoDispatch>(apply_transmission_gamma_c_algo)
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::CopyYForInterpolation(MuseBuffer<float> &frame, MuseBuffer<float> &output,
                                    unsigned int field_parity, unsigned int frame_phase_y) {
    std::shared_ptr<kp::Algorithm> copy_y_for_interpolation_algo =
            m_mgr.algorithm({frame.tensor(), output.tensor()},
                            m_copy_y_for_interpolation_spirv, kp::Workgroup({MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {0, 0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({frame.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    copy_y_for_interpolation_algo,
                    std::vector{field_parity, frame_phase_y})
            ->record<kp::OpTensorSyncLocal>({output.tensor()})
            ->eval();
}

void Shaders::FilterImageDiamond(int phase, MuseBuffer<float> &buffer) {
    std::shared_ptr<kp::Algorithm> diamond_algo =
            m_mgr.algorithm({m_diamond_filter_buffer.tensor(), buffer.tensor()},
                            m_diamond_spirv, kp::Workgroup({buffer.width(), buffer.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({m_diamond_filter_buffer.tensor(), buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    diamond_algo,
                    std::vector{m_diamond_filter_buffer.height(), m_diamond_filter_buffer.width(), buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::FilterImage(unsigned int filter_height, unsigned int filter_width, std::shared_ptr<kp::Tensor> &filter,
                          MuseBuffer<float> &source, MuseBuffer<float> &dest,
                          float border_value, float multiplier) {
    assert(source.height() == dest.height());
    assert(source.width() == dest.width());

    std::shared_ptr<kp::Algorithm> filter_image_algo =
            m_mgr.algorithm({filter, source.tensor(), dest.tensor()},
                            m_filter_image_spirv, kp::Workgroup({source.width(), source.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({filter, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    filter_image_algo,
                    std::vector{(float)filter_height, (float)filter_width, (float)source.height(), (float)source.width(), border_value, multiplier})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()})
            ->eval();
}

void Shaders::ConvertHorizSampleRate2to3(MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                         unsigned int output_start_row, unsigned int output_row_step_size) {
    assert(source.width() * 3 == dest.width() * 2);
    assert(source.height() * output_row_step_size == dest.height());

    std::shared_ptr<kp::Algorithm> convert_2_to_3_algo =
            m_mgr.algorithm({m_filter_2_to_3_tensor, source.tensor(), dest.tensor()},
                            m_convert_2_to_3_spirv, kp::Workgroup({dest.width(), dest.height() / output_row_step_size}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({m_filter_2_to_3_tensor, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    convert_2_to_3_algo,
                    std::vector{m_filter_2_to_3_tensor->size(), source.height(), source.width(), output_start_row, output_row_step_size})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()})
            ->eval();
}

void Shaders::FillEmptyLines(MuseBuffer<float> &buffer, int phase) {
    std::shared_ptr<kp::Algorithm> fill_empty_lines_algo =
            m_mgr.algorithm({buffer.tensor()},
                            m_fill_empty_lines_spirv, kp::Workgroup({buffer.width(), buffer.height() / 2}),
                            {}, {0.0, 0.0, 0.0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    fill_empty_lines_algo,
                    std::vector{buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()})
            ->eval();
}

void Shaders::CombineYandRB(MuseBuffer<float> &Y_data, MuseBuffer<float> &C_r_data,
                            MuseBuffer<float> &C_b_data, MuseBuffer<uint32_t> &out_rgb) {
    std::shared_ptr<kp::Algorithm> combine_y_and_rb_algo =
            m_mgr.algorithm({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor(), out_rgb.tensor()},
                            m_combine_y_and_rb_spirv, kp::Workgroup({Y_data.width(), Y_data.height()}),
                            {}, {0, 0, 0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    combine_y_and_rb_algo,
                    std::vector{Y_data.height(), Y_data.width(), C_r_data.width()})
            ->record<kp::OpTensorSyncLocal>({out_rgb.tensor()})
            ->eval();
}

void Shaders::DecodeC(MuseBuffer<float> &input_frame, MuseBuffer<float> &C_r_data,
                      MuseBuffer<float> &C_b_data, int frame_phase_c, int field_parity) {
    std::shared_ptr<kp::Algorithm> decode_c_algo =
            m_mgr.algorithm({input_frame.tensor(), C_r_data.tensor(), C_b_data.tensor()},
                            m_decode_c_spirv, kp::Workgroup({C_r_data.width(), C_r_data.height()}),
                            {}, {0, 0});
    m_mgr.sequence()
            ->record<kp::OpTensorSyncDevice>({input_frame.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    decode_c_algo,
                    std::vector{frame_phase_c, field_parity})
            ->record<kp::OpTensorSyncLocal>({C_r_data.tensor(), C_b_data.tensor()})
            ->eval();
}

