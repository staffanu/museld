//
// Created by staffanu on 5/6/23.
//

#include <stdexcept>
#include <fstream>
#include <iostream>
#include <opencv2/highgui.hpp>
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

void show_buffer(int y_min, int x_min, int height, int width, int x_scale, const MuseBuffer<float> &buffer) {
    cv::Mat mat(height, width * x_scale, CV_32FC1);
    for (int row = y_min; row < y_min + height; row++) {
        auto *mat_row = mat.ptr<float>(row - y_min);
        for (int col = x_min; col < x_min + width * x_scale; col++) {
            mat_row[col - x_min] = buffer[row][col / x_scale] / 255.0f;
        }
    }
    cv::imshow("MUSE", mat);
    cv::waitKey(0);
}

Shaders::Shaders()
: m_mgr(kp::Manager(0, {}, { "VK_EXT_shader_atomic_float" })),
  m_interpolated32_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_intermediate_r_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_Y_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_r_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_b_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_Y_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_inter_frame_r_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_b_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_field_buffers {
          Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH / 2),
          Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH / 2),
          Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH / 2),
  },
  m_movement_buffer(Shaders::CreateMuseBuffer<float>(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
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
          }))),
  m_color_filter_single_field_buffer(MuseBuffer<float>(5, 9, m_mgr.tensor(
          {
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.008117, 0.021787, 0.054789, 0.087791, 0.101461, 0.087791, 0.054789, 0.021787, 0.008117,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
          }))),
  m_color_filter_inter_frame_buffer(MuseBuffer<float>(5, 5, m_mgr.tensor(
          {
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.001980, 0.091620, 0.259228, 0.091620, 0.001980,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
          })))
{
    m_diamond_spirv = CompileSource("../../musecpp/shaders/filter_diamond.comp");
    m_filter_image_spirv = CompileSource("../../musecpp/shaders/filter_image.comp");
    m_apply_transmission_gamma_y_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_y.comp");
    m_apply_transmission_gamma_c_spirv = CompileSource("../../musecpp/shaders/apply_transmission_gamma_c.comp");
    m_copy_y_for_interpolation_spirv = CompileSource("../../musecpp/shaders/copy_y_for_interpolation.comp");
    m_convert_horiz_sample_rate_spirv = CompileSource("../../musecpp/shaders/convert_horiz_sample_rate.comp");
    m_fill_empty_lines_spirv = CompileSource("../../musecpp/shaders/fill_empty_lines.comp");
    m_combine_y_and_rb_spirv = CompileSource("../../musecpp/shaders/combine_y_and_rb.comp");
    m_decode_c_spirv = CompileSource("../../musecpp/shaders/decode_c.comp");
    m_detect_motion_spirv = CompileSource("../../musecpp/shaders/detect_motion.comp");
    m_combine_still_and_moving_spirv = CompileSource("../../musecpp/shaders/combine_still_and_moving.comp");

    m_filter_2_to_3_tensor = m_mgr.tensor(
            {
                    // cutoff 0.25, transition 0.05, sampling freq 1, Rectangular: 19 coeffs (25 non-zero).  Looks +/- 9 samples in each direction
                    0.034286805542972851, -0.000000000000000019, -0.044083035698107946, 0.000000000000000019, 0.061716249977351118,-0.000000000000000019, -0.102860416628918538,
                    0.000000000000000019, 0.308581249886755615, 0.484718293839893788, 0.308581249886755615, 0.000000000000000019, -0.102860416628918538, -0.000000000000000019,
                    0.061716249977351118, 0.000000000000000019, -0.044083035698107946, -0.000000000000000019, 0.034286805542972851
            });
    m_filter_4_to_3_tensor = m_mgr.tensor(
            {
                    // cutoff 0.125, transition 0.03, sampling freq 1, Rectangular: 31 coeffs (25 non-zero).  Looks +/- 15 samples in each direction
                    -0.015752415092402161, -0.023868513282649006, -0.018175863568156325, 0.000000000000000010, 0.021480566035093858, 0.033415918595708603, 0.026254025154003564, -0.000000000000000010,
                    -0.033755175198004597, -0.055693197659514346, -0.047257245277206421, 0.000000000000000010, 0.078762075462010708, 0.167079592978543023, 0.236286226386032083, 0.262448010933081788,
                    0.236286226386032083, 0.167079592978543023, 0.078762075462010708, 0.000000000000000010, -0.047257245277206421, -0.055693197659514346, -0.033755175198004597, -0.000000000000000010,
                    0.026254025154003564, 0.033415918595708603, 0.021480566035093858, 0.000000000000000010, -0.018175863568156325, -0.023868513282649006, -0.015752415092402161
            });
    m_filter_4_to_1_tensor = m_mgr.tensor(
            {
                    // cutoff 0.125, transition 0.03, sampling freq 1, Rectangular: 31 coeffs (25 non-zero).  Looks +/- 15 samples in each direction
                    -0.015752415092402161, -0.023868513282649006, -0.018175863568156325, 0.000000000000000010, 0.021480566035093858, 0.033415918595708603, 0.026254025154003564, -0.000000000000000010,
                    -0.033755175198004597, -0.055693197659514346, -0.047257245277206421, 0.000000000000000010, 0.078762075462010708, 0.167079592978543023, 0.236286226386032083, 0.262448010933081788,
                    0.236286226386032083, 0.167079592978543023, 0.078762075462010708, 0.000000000000000010, -0.047257245277206421, -0.055693197659514346, -0.033755175198004597, -0.000000000000000010,
                    0.026254025154003564, 0.033415918595708603, 0.021480566035093858, 0.000000000000000010, -0.018175863568156325, -0.023868513282649006, -0.015752415092402161
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

cv::Mat Shaders::DecodeIntraField(FieldBufferView &field) {
    cout << "Decoding frame " << field.m_frame_no << " field " << field.m_field_parity << endl;
    if (field.m_control.has_value())
        field.m_control.value().print_control_data();

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.m_control.has_value() ?
                        field.m_control.value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.m_control.has_value() ?
                        field.m_control.value().frame_subsampling_phase_C.value_or(0) : 0;
    // int field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);

    auto sq = m_mgr.sequence();

    CopyYForInterpolation(sq, *field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y, true);
    FilterImageDiamond(sq, frame_phase_y, m_interpolated32_buffer);
    ConvertHorizSampleRate2to3(sq, m_interpolated32_buffer, m_field_Y_buffer, 1 - field_parity);
    FillEmptyLines(sq, m_field_Y_buffer, field_parity);

    DecodeC(sq, *field.m_data, m_intermediate_r_buffer, m_intermediate_b_buffer,
                    frame_phase_c, field_parity, true);
    FilterImage(sq, m_color_filter_single_field_buffer, m_intermediate_r_buffer, m_field_r_buffer, 128.0 / 8, 8);
    FilterImage(sq, m_color_filter_single_field_buffer, m_intermediate_b_buffer, m_field_b_buffer, 128.0 / 8, 8);

    CombineYandRB(sq, m_field_Y_buffer, m_field_r_buffer, m_field_b_buffer, m_frame_out_buffer);

    sq->eval();

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

void Shaders::CopyYForInterpolation(shared_ptr<kp::Sequence> const &sq,
        MuseBuffer<float> &frame, MuseBuffer<float> &output,
        unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries) {
    std::shared_ptr<kp::Algorithm> copy_y_for_interpolation_algo =
            m_mgr.algorithm({frame.tensor(), output.tensor()},
                            m_copy_y_for_interpolation_spirv, kp::Workgroup({MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT}),
                            {}, {0, 0, 0});
    sq->record<kp::OpTensorSyncDevice>({frame.tensor(), output.tensor()}) // FIXME: output doesn't have to be synced
            ->record<kp::OpAlgoDispatch>(
                    copy_y_for_interpolation_algo,
                    std::vector{field_parity, frame_phase_y, zero_non_copied_entries ? 1u : 0u})
            ->record<kp::OpTensorSyncLocal>({output.tensor()});
}

void Shaders::FilterImageDiamond(std::shared_ptr<kp::Sequence> const &sq,
                                 int phase, MuseBuffer<float> &buffer) {
    std::shared_ptr<kp::Algorithm> diamond_algo =
            m_mgr.algorithm({m_diamond_filter_buffer.tensor(), buffer.tensor()},
                            m_diamond_spirv, kp::Workgroup({buffer.width(), buffer.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0});
    sq->record<kp::OpTensorSyncDevice>({m_diamond_filter_buffer.tensor(), buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    diamond_algo,
                    std::vector{m_diamond_filter_buffer.height(), m_diamond_filter_buffer.width(), buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()});
}

void Shaders::FilterImage(std::shared_ptr<kp::Sequence> const &sq,
                          MuseBuffer<float> &filter,
                          MuseBuffer<float> &source, MuseBuffer<float> &dest,
                          float border_value, float multiplier) {
    assert(source.height() == dest.height());
    assert(source.width() == dest.width());

    std::shared_ptr<kp::Algorithm> filter_image_algo =
            m_mgr.algorithm({filter.tensor(), source.tensor(), dest.tensor()},
                            m_filter_image_spirv, kp::Workgroup({source.width(), source.height()}),
                            {}, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    sq->record<kp::OpTensorSyncDevice>({filter.tensor(), source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    filter_image_algo,
                    std::vector{(float)filter.height(), (float)filter.width(), (float)source.height(), (float)source.width(), border_value, multiplier})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()});
}

void Shaders::ConvertHorizSampleRate2to3(std::shared_ptr<kp::Sequence> const &sq,
                                         MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                         unsigned int output_start_row) {
    assert(source.width() * 3 == dest.width() * 2);
    assert(source.height() * 2 == dest.height());

    std::shared_ptr<kp::Algorithm> convert_2_to_3_algo =
            m_mgr.algorithm({m_filter_2_to_3_tensor, source.tensor(), dest.tensor()},
                            m_convert_horiz_sample_rate_spirv, kp::Workgroup({dest.width(), dest.height() / 2}),
                            {}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    sq->record<kp::OpTensorSyncDevice>({m_filter_2_to_3_tensor, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    convert_2_to_3_algo,
                    std::vector{m_filter_2_to_3_tensor->size(), 3u, 2u, source.height(), source.width(), output_start_row, 2u, 0u, 1u, 1u})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()});
}

void Shaders::ConvertHorizSampleRate4to3(std::shared_ptr<kp::Sequence> const &sq,
                                         MuseBuffer<float> &source, MuseBuffer<float> &dest,
                                         unsigned int output_start_row, unsigned int output_start_col) {
    assert(source.width() * 3 == dest.width() * 2);
    assert(source.height() * 2 == dest.height());

    std::shared_ptr<kp::Algorithm> convert_4_to_3_algo =
            m_mgr.algorithm({m_filter_4_to_3_tensor, source.tensor(), dest.tensor()},
                            m_convert_horiz_sample_rate_spirv, kp::Workgroup({dest.width(), dest.height() / 2}),
                            {}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    sq->record<kp::OpTensorSyncDevice>({m_filter_4_to_3_tensor, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    convert_4_to_3_algo,
                    std::vector{m_filter_4_to_3_tensor->size(), 3u, 4u, source.height(), source.width(), output_start_row, 2u, output_start_col, 2u, 1u})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()});
}

void Shaders::ConvertHorizSampleRate4to1(std::shared_ptr<kp::Sequence> const &sq,
                                         MuseBuffer<float> &source, MuseBuffer<float> &dest) {
    assert(source.width() == dest.width() * 4);
    assert(source.height() == dest.height());

    std::shared_ptr<kp::Algorithm> convert_4_to_3_algo =
            m_mgr.algorithm({m_filter_4_to_3_tensor, source.tensor(), dest.tensor()},
                            m_convert_horiz_sample_rate_spirv, kp::Workgroup({dest.width(), dest.height() / 2}),
                            {}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    sq->record<kp::OpTensorSyncDevice>({m_filter_4_to_3_tensor, source.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    convert_4_to_3_algo,
                    std::vector{m_filter_4_to_1_tensor->size(), 1u, 4u, source.height(), source.width(), 0u, 1u, 0u, 1u, 2u})
            ->record<kp::OpTensorSyncLocal>({dest.tensor()});
}

void Shaders::FillEmptyLines(std::shared_ptr<kp::Sequence> const &sq, MuseBuffer<float> &buffer, int phase) {
    std::shared_ptr<kp::Algorithm> fill_empty_lines_algo =
            m_mgr.algorithm({buffer.tensor()},
                            m_fill_empty_lines_spirv, kp::Workgroup({buffer.width(), buffer.height() / 2}),
                            {}, {0.0, 0.0, 0.0});
    sq->record<kp::OpTensorSyncDevice>({buffer.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    fill_empty_lines_algo,
                    std::vector{buffer.height(), buffer.width(), (unsigned)phase})
            ->record<kp::OpTensorSyncLocal>({buffer.tensor()});
}

void Shaders::CombineYandRB(std::shared_ptr<kp::Sequence> const &sq,
                            MuseBuffer<float> &Y_data, MuseBuffer<float> &C_r_data,
                            MuseBuffer<float> &C_b_data, MuseBuffer<uint32_t> &out_rgb) {
    std::shared_ptr<kp::Algorithm> combine_y_and_rb_algo =
            m_mgr.algorithm({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor(), out_rgb.tensor()},
                            m_combine_y_and_rb_spirv, kp::Workgroup({Y_data.width(), Y_data.height()}),
                            {}, {0, 0, 0});
    sq->record<kp::OpTensorSyncDevice>({Y_data.tensor(), C_r_data.tensor(), C_b_data.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    combine_y_and_rb_algo,
                    std::vector{Y_data.height(), Y_data.width(), C_r_data.width()})
            ->record<kp::OpTensorSyncLocal>({out_rgb.tensor()});
}

void Shaders::DecodeC(std::shared_ptr<kp::Sequence> const &sq,
                      MuseBuffer<float> &input_frame,
                      MuseBuffer<float> &C_r_data, MuseBuffer<float> &C_b_data,
                      int frame_phase_c, int field_parity, bool zero_non_sample_points) {
    std::shared_ptr<kp::Algorithm> decode_c_algo =
            m_mgr.algorithm({input_frame.tensor(), C_r_data.tensor(), C_b_data.tensor()},
                            m_decode_c_spirv, kp::Workgroup({C_r_data.width(), C_r_data.height()}),
                            {}, {0, 0, 0});
    // The output tensors are also inputs since the shader modifies their data.
    // We do not need to sync them, however, since they are not written by the CPU.
    sq->record<kp::OpTensorSyncDevice>({input_frame.tensor()})
            ->record<kp::OpAlgoDispatch>(
                    decode_c_algo,
                    std::vector{frame_phase_c, field_parity, zero_non_sample_points ? 1 : 0})
            ->record<kp::OpTensorSyncLocal>({C_r_data.tensor(), C_b_data.tensor()});
}

optional<vector<vector<int>>>
check_control_signal_data(vector<reference_wrapper<FieldBufferView>> const &fields) {
    if (!all_of(fields.cbegin(), fields.cend(),
                [](const reference_wrapper<FieldBufferView> f) -> bool {
                    return f.get().m_control.has_value() &&
                           f.get().m_control.value().field_subsampling_phase_Y.has_value() &&
                           f.get().m_control.value().frame_subsampling_phase_Y.has_value() &&
                           f.get().m_control.value().frame_subsampling_phase_C.has_value();
                })) {
        cout << "Unknown phases for inter frame interpolation" << endl;
        return nullopt;
    }

    vector<int> field_parities = vector<int> {
            fields[0].get().m_field_parity,
            fields[1].get().m_field_parity,
            fields[2].get().m_field_parity,
            fields[3].get().m_field_parity };
    vector<int> frame_phases_y = vector<int> {
            fields[0].get().m_control.value().frame_subsampling_phase_Y.value(),
            fields[1].get().m_control.value().frame_subsampling_phase_Y.value(),
            fields[2].get().m_control.value().frame_subsampling_phase_Y.value(),
            fields[3].get().m_control.value().frame_subsampling_phase_Y.value() };
    vector<int> field_phases_y = vector<int> {
            fields[0].get().m_control.value().field_subsampling_phase_Y.value(),
            fields[1].get().m_control.value().field_subsampling_phase_Y.value(),
            fields[2].get().m_control.value().field_subsampling_phase_Y.value(),
            fields[3].get().m_control.value().field_subsampling_phase_Y.value() };
    vector<int> frame_phases_c = vector<int> {
            fields[0].get().m_control.value().frame_subsampling_phase_C.value(),
            fields[1].get().m_control.value().frame_subsampling_phase_C.value(),
            fields[2].get().m_control.value().frame_subsampling_phase_C.value(),
            fields[3].get().m_control.value().frame_subsampling_phase_C.value() };

    if (!(field_parities[0] == field_parities[2] && field_parities[1] == field_parities[3] && field_parities[0] != field_parities[1]) ||
        !(field_phases_y[0] == field_phases_y[2] && field_phases_y[1] == field_phases_y[3] && field_phases_y[0] != field_phases_y[1]) ||
        !(frame_phases_y[0] != frame_phases_y[2] && frame_phases_y[1] != frame_phases_y[3])) {
        cout << "Inconsistent phases for inter frame interpolation" << endl;
        return nullopt;
    }
    return {{field_parities, frame_phases_y, field_phases_y, frame_phases_c}};
}

// There are 4 fields in the vector.  Index 0 is the newest.
optional<cv::Mat> Shaders::DecodeInterFrame(vector<reference_wrapper<FieldBufferView>> const &fields) {
    assert(fields.size() >= 4); // It is actually 5, but we only use 4 here, the 5th field is used for motion detection
    auto control_signal_data = check_control_signal_data(fields);
    if (!control_signal_data.has_value())
        return nullopt;
    auto tuple = control_signal_data.value();
    vector<int> field_parities = tuple[0];
    vector<int> frame_phases_y = tuple[1];
    vector<int> field_phases_y = tuple[2];
    vector<int> frame_phases_c = tuple[3];

    auto sq = m_mgr.sequence();

    memset(m_inter_frame_Y_buffer.data(), 0, m_inter_frame_Y_buffer.byte_size());
    sq->record<kp::OpTensorSyncDevice>({m_inter_frame_Y_buffer.tensor()});
    MakeFieldFromConsecutiveFrames(sq, fields[0], frame_phases_y[0], fields[2], frame_phases_y[2], field_parities[0], field_phases_y[0]);
    MakeFieldFromConsecutiveFrames(sq, fields[1], frame_phases_y[1], fields[3], frame_phases_y[3], field_parities[1], field_phases_y[1]);
    FilterImageDiamond(sq, 1 ^ field_parities[0] ^ field_phases_y[0], m_inter_frame_Y_buffer);

    for (int i = 0; i < 4; i++)
        DecodeC(sq, *fields[i].get().m_data, m_intermediate_r_buffer, m_intermediate_b_buffer,
                frame_phases_c[i], field_parities[i], i == 0);
    FilterImage(sq, m_color_filter_inter_frame_buffer, m_intermediate_r_buffer, m_inter_frame_r_buffer, 128.0 / 2, 2);
    FilterImage(sq, m_color_filter_inter_frame_buffer, m_intermediate_b_buffer, m_inter_frame_b_buffer, 128.0 / 2, 2);

    CombineYandRB(sq, m_inter_frame_Y_buffer, m_inter_frame_r_buffer, m_inter_frame_b_buffer, m_frame_out_buffer);

    sq->eval();

//    size_t steps[1] = { MUSE_Y_BUF_WIDTH * 12 };
//    cv::Mat mat(vector<int>{ 120, 280 }, CV_8UC4,
//                m_frame_out_buffer.data() + MUSE_Y_BUF_WIDTH * 3 * 330,
//                steps);
//    cv::namedWindow("MUSE", cv::WINDOW_NORMAL);
//    cv::resizeWindow("MUSE", 16 * 100, 9 * 100);
//    cv::imshow("MUSE", mat);
//    cv::waitKey(0);

    return {cv::Mat(vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_8UC4, m_frame_out_buffer.data())};
}

void Shaders::MakeFieldFromConsecutiveFrames(std::shared_ptr<kp::Sequence> const &sq,
                                             FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                             FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                             unsigned int fields_parity, unsigned int fields_phases) {
    CopyYForInterpolation(sq, *field_a.m_data, m_interpolated32_buffer, fields_parity, field_a_frame_phase_y, false);
    CopyYForInterpolation(sq, *field_b.m_data, m_interpolated32_buffer, fields_parity, field_b_frame_phase_y, false);
    ConvertHorizSampleRate4to3(sq, m_interpolated32_buffer, m_inter_frame_Y_buffer, 1 - fields_parity, 1 - fields_phases);
}

std::optional<cv::Mat> Shaders::DetectMotion(std::vector<std::reference_wrapper<FieldBufferView>> const &fields) {
    assert(fields.size() >= 5);
    auto control_signal_data = check_control_signal_data(fields);
    if (!control_signal_data.has_value())
        return nullopt;
    auto tuple = control_signal_data.value();
    vector<int> field_parities = tuple[0];
    vector<int> frame_phases_y = tuple[1];
    vector<int> field_phases_y = tuple[2];
    vector<int> frame_phases_c = tuple[3];

    auto sq = m_mgr.sequence();

    for (int i = 0; i < 3; i++) {
        CopyYForInterpolation(sq, *fields[i * 2].get().m_data, m_interpolated32_buffer, field_parities[i * 2],
                              frame_phases_y[i * 2], true);
        ConvertHorizSampleRate4to1(sq, m_interpolated32_buffer, m_movement_field_buffers[i]);
    }

//    sq->eval();
//    for (auto &m_movement_field_buffer : m_movement_field_buffers) {
//        show_buffer(0, 0, MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH / 2, 4, m_movement_field_buffer);
//    }

    std::shared_ptr<kp::Algorithm> detect_motion_algo =
            m_mgr.algorithm({m_movement_field_buffers[0].tensor(), m_movement_field_buffers[1].tensor(),
                             m_movement_field_buffers[2].tensor(), m_movement_buffer.tensor()},
                            m_detect_motion_spirv, kp::Workgroup({m_movement_buffer.width(), m_movement_buffer.height()}),
                            {}, {});
    sq->record<kp::OpAlgoDispatch>(detect_motion_algo)
            ->record<kp::OpTensorSyncLocal>({m_movement_buffer.tensor()});

    sq->eval();

    return {cv::Mat(vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_32FC1, m_movement_buffer.data())};
}

optional<cv::Mat> Shaders::CombineStillAndMovingParts() {
    auto sq = m_mgr.sequence();

    std::shared_ptr<kp::Algorithm> combine_still_and_moving_algo =
            m_mgr.algorithm({m_field_Y_buffer.tensor(), m_field_r_buffer.tensor(),
                             m_field_b_buffer.tensor(), m_inter_frame_Y_buffer.tensor(),
                             m_inter_frame_r_buffer.tensor(), m_inter_frame_b_buffer.tensor(),
                             m_movement_buffer.tensor(), m_frame_out_buffer.tensor()},
                            m_combine_still_and_moving_spirv, kp::Workgroup({m_frame_out_buffer.width(), m_frame_out_buffer.height()}),
                            {}, {});

    sq->record<kp::OpAlgoDispatch>(combine_still_and_moving_algo)
            ->record<kp::OpTensorSyncLocal>({m_frame_out_buffer.tensor()});

    sq->eval();

    return {cv::Mat(vector<int>{ MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3}, CV_8UC4, m_frame_out_buffer.data())};
}
