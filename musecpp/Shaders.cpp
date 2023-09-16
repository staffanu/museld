//
// Created by staffanu on 5/6/23.
//

#include <stdexcept>
#include <fstream>
#include <fmt/format.h>
#include "MuseTypes.h"
#include "MuseBuffer.h"
#include "Shaders.h"
#include "FieldBufferView.h"
#include "musevk/VulkanManager.h"

using namespace std;
using namespace musevk;

vector<uint32_t> Shaders::loadSpirv(string const &executable_dir, string const &filename) {
    string full_path = executable_dir + "/shaders/" + filename + ".spv";
    ifstream file_stream(full_path, ios::binary);
    if (file_stream.fail())
        throw std::runtime_error("Unable to open shader spirv file " + full_path);
    vector<char> buffer;
    buffer.insert(buffer.begin(), istreambuf_iterator<char>(file_stream), {});
    return {(uint32_t*)buffer.data(), (uint32_t*)(buffer.data() + buffer.size())};
}

Shaders::Shaders(Logger &log, std::string const &executable_dir, VulkanManager &manager)
: m_log(log),
  m_vulkan_manager(manager),
  m_interpolated32_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_intermediate_r_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_Y_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_r_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_b_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_Y_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_inter_frame_r_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_b_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_movement_edge_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_coring_buffer(Shaders::createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_image_out(m_vulkan_manager.createImage(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2)),
  m_diamond_filter_buffer(MuseBuffer(7, 9, m_vulkan_manager.createDeviceBuffer(
          {
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
          }))),
  m_color_filter_single_field_buffer(MuseBuffer(5, 9, m_vulkan_manager.createDeviceBuffer(
          {
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.008117, 0.021787, 0.054789, 0.087791, 0.101461, 0.087791, 0.054789, 0.021787, 0.008117,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
          }))),
  m_color_filter_inter_frame_buffer(MuseBuffer(5, 5, m_vulkan_manager.createDeviceBuffer(
          {
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.001980, 0.091620, 0.259228, 0.091620, 0.001980,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
          }))),
  m_filter_2_to_3_buffer(m_vulkan_manager.createDeviceBuffer(
          {
                  // cutoff 0.25, transition 0.05, sampling freq 1, Rectangular: 19 coeffs (25 non-zero).  Looks +/- 9 samples in each direction
                  0.034286805542972851, -0.000000000000000019, -0.044083035698107946, 0.000000000000000019,
                  0.061716249977351118, -0.000000000000000019, -0.102860416628918538,
                  0.000000000000000019, 0.308581249886755615, 0.484718293839893788, 0.308581249886755615,
                  0.000000000000000019, -0.102860416628918538, -0.000000000000000019,
                  0.061716249977351118, 0.000000000000000019, -0.044083035698107946, -0.000000000000000019,
                  0.034286805542972851
          })),
  m_filter_4_to_3_buffer(m_vulkan_manager.createDeviceBuffer(
          {
                  // cutoff 0.125, transition 0.03, sampling freq 1, Rectangular: 31 coeffs (25 non-zero).  Looks +/- 15 samples in each direction
                  -0.015752415092402161, -0.023868513282649006, -0.018175863568156325, 0.000000000000000010,
                  0.021480566035093858, 0.033415918595708603, 0.026254025154003564, -0.000000000000000010,
                  -0.033755175198004597, -0.055693197659514346, -0.047257245277206421, 0.000000000000000010,
                  0.078762075462010708, 0.167079592978543023, 0.236286226386032083, 0.262448010933081788,
                  0.236286226386032083, 0.167079592978543023, 0.078762075462010708, 0.000000000000000010,
                  -0.047257245277206421, -0.055693197659514346, -0.033755175198004597, -0.000000000000000010,
                  0.026254025154003564, 0.033415918595708603, 0.021480566035093858, 0.000000000000000010,
                  -0.018175863568156325, -0.023868513282649006, -0.015752415092402161
          })),
  m_filter_4_to_1_buffer(m_vulkan_manager.createDeviceBuffer(
          {
                  // cutoff 0.125, transition 0.03, sampling freq 1, Rectangular: 31 coeffs (25 non-zero).  Looks +/- 15 samples in each direction
                  -0.015752415092402161, -0.023868513282649006, -0.018175863568156325, 0.000000000000000010,
                  0.021480566035093858, 0.033415918595708603, 0.026254025154003564, -0.000000000000000010,
                  -0.033755175198004597, -0.055693197659514346, -0.047257245277206421, 0.000000000000000010,
                  0.078762075462010708, 0.167079592978543023, 0.236286226386032083, 0.262448010933081788,
                  0.236286226386032083, 0.167079592978543023, 0.078762075462010708, 0.000000000000000010,
                  -0.047257245277206421, -0.055693197659514346, -0.033755175198004597, -0.000000000000000010,
                  0.026254025154003564, 0.033415918595708603, 0.021480566035093858, 0.000000000000000010,
                  -0.018175863568156325, -0.023868513282649006, -0.015752415092402161
          })),
  m_audio_data(createMuseBuffer(88, MUSE_TOTAL_WIDTH * 3 / 4, true))
{
    m_convert_to_float_and_apply_eq_and_gamma_spriv = loadSpirv(executable_dir, "convert_to_float_and_apply_eq_and_gamma.comp");
    m_diamond_spirv = loadSpirv(executable_dir, "filter_diamond.comp");
    m_filter_image_spirv = loadSpirv(executable_dir, "filter_image.comp");
    m_copy_y_for_interpolation_spirv = loadSpirv(executable_dir, "copy_y_for_interpolation.comp");
    m_convert_horiz_sample_rate_spirv = loadSpirv(executable_dir, "convert_horiz_sample_rate.comp");
    m_fill_empty_lines_spirv = loadSpirv(executable_dir, "fill_empty_lines.comp");
    m_decode_c_spirv = loadSpirv(executable_dir, "decode_c.comp");
    m_detect_motion_spirv = loadSpirv(executable_dir, "detect_motion.comp");
    m_combine_still_and_moving_spirv = loadSpirv(executable_dir, "combine_still_and_moving.comp");
    
    m_convert_to_float_and_apply_eq_and_gamma_algo = m_vulkan_manager.createComputeShader(
            "convert_to_float_and_apply_eq_and_gamma",
            {eBuffer, eBuffer}, sizeof(float) * 2,
            m_convert_to_float_and_apply_eq_and_gamma_spriv, Workgroup(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT));
    m_copy_y_for_interpolation_algo = m_vulkan_manager.createComputeShader(
            "copy_y_for_interpolation",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            m_copy_y_for_interpolation_spirv, Workgroup(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT), 8);
    m_diamond_algo = m_vulkan_manager.createComputeShader(
            "diamond",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 5,
            m_diamond_spirv, Workgroup(0), 2);
    m_filter_image_algo = m_vulkan_manager.createComputeShader(
            "filter_image",
            {eBuffer, eBuffer, eBuffer}, sizeof(float) * 6,
            m_filter_image_spirv, Workgroup(0), 4);
    m_fill_empty_lines_algo = m_vulkan_manager.createComputeShader(
            "fill_empty_lines",
            {m_field_Y_buffer.getVulkanBuffer()}, sizeof(uint32_t) * 3,
            m_fill_empty_lines_spirv, Workgroup(m_field_Y_buffer.width(), m_field_Y_buffer.height() / 2));
    m_convert_sample_rate_algo = m_vulkan_manager.createComputeShader(
            "convert_sample_rate",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Workgroup(m_interpolated32_buffer.width() / 4, m_interpolated32_buffer.height()), 3);
    m_convert_sample_rate_4_to_3_algo = m_vulkan_manager.createComputeShader(
            "convert_sample_rate_4_to_3",
            {m_filter_4_to_3_buffer, m_interpolated32_buffer.getVulkanBuffer(),
             m_inter_frame_Y_buffer.getVulkanBuffer()}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Workgroup(m_inter_frame_Y_buffer.width(), m_inter_frame_Y_buffer.height() / 2));
    m_convert_sample_rate_2_to_3_algo = m_vulkan_manager.createComputeShader(
            "convert_sample_rate_2_to_3",
            {m_filter_2_to_3_buffer, m_interpolated32_buffer.getVulkanBuffer(), m_field_Y_buffer.getVulkanBuffer()},
            sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Workgroup(m_field_Y_buffer.width(), m_field_Y_buffer.height() / 2));
    m_decode_c_algo = m_vulkan_manager.createComputeShader(
            "decode_c",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            m_decode_c_spirv, Workgroup(m_intermediate_r_buffer.width(), m_intermediate_r_buffer.height()), 5);
    m_detect_motion_algo = m_vulkan_manager.createComputeShader(
            "detect_motion",
            {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t),
            m_detect_motion_spirv, Workgroup(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT));
    m_combine_still_and_moving_algo = m_vulkan_manager.createComputeShader(
            "combine_still_and_moving",
            {m_field_Y_buffer.getVulkanBuffer(), m_field_r_buffer.getVulkanBuffer(),
             m_field_b_buffer.getVulkanBuffer(), m_inter_frame_Y_buffer.getVulkanBuffer(),
             m_inter_frame_r_buffer.getVulkanBuffer(), m_inter_frame_b_buffer.getVulkanBuffer(),
             m_movement_buffer.getVulkanBuffer(), m_image_out},
            sizeof(uint32_t) * 2,
            m_combine_still_and_moving_spirv,
            Workgroup(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2));
    m_convert_audio_sample_rate_algo = m_vulkan_manager.createComputeShader(
            "convert_audio_sample_rate",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Workgroup(MUSE_TOTAL_WIDTH, 44), 2);
}

MuseBuffer Shaders::createMuseBuffer(unsigned int height, unsigned int width, bool host_visible) {
    auto vulkan_buffer = m_vulkan_manager.createBuffer(height * width, 2 /* sizeof(float16) */, host_visible, false);
    auto buffer = MuseBuffer(height, width, std::move(vulkan_buffer));
    return buffer;
}

void Shaders::convertToFloatAndApplyEqAndGamma(
        CommandQueue &sq, shared_ptr<VulkanBuffer> input,
        MuseBuffer &buffer, pair<float, float> const &eq) {
    m_convert_to_float_and_apply_eq_and_gamma_algo->updateBufferDescriptorsInSet(0, {input, buffer.getVulkanBuffer()});
    sq.enqueueComputeShader(m_convert_to_float_and_apply_eq_and_gamma_algo, {eq.first, eq.second});
}

void Shaders::decodeIntraField(CommandQueue &sq, FieldBufferView &field) {
    m_log.info(eVideo, fmt::format("Decoding frame {} field {}", field.m_frame_no, field.m_field_parity));
    if (field.control_data().has_value())
        field.control_data().value().log_control_data();

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.control_data().has_value() ?
                        field.control_data().value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.control_data().has_value() ?
                        field.control_data().value().frame_subsampling_phase_C.value_or(0) : 0;
    // int field_phase = field.m_control.value().field_subsampling_phase_Y.value_or(0);

    copyYForInterpolation(sq, 0, field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y, true);
    filterImageDiamond(sq, 0, frame_phase_y, m_interpolated32_buffer);

    sq.enqueueComputeShader(
            m_convert_sample_rate_2_to_3_algo,
            vector{m_filter_2_to_3_buffer->size(), 3u, 2u, 0u, 0u, m_interpolated32_buffer.height(), m_interpolated32_buffer.width(),
                   uint(1 - field_parity), 2u, 0u, 1u, 0u, 1u});

    sq.enqueueComputeShader(m_fill_empty_lines_algo,
                            vector{m_field_Y_buffer.height(), m_field_Y_buffer.width(), (unsigned)field_parity});

    decodeC(sq, 0, field.m_data, frame_phase_c, field_parity, true);
    filterImage(sq, 0, m_color_filter_single_field_buffer, m_intermediate_r_buffer, m_field_r_buffer, 128.0 / 8, 8);
    filterImage(sq, 1, m_color_filter_single_field_buffer, m_intermediate_b_buffer, m_field_b_buffer, 128.0 / 8, 8);
}

void Shaders::copyYForInterpolation(CommandQueue &sq, int descriptor_set_index,
                                    MuseBuffer &frame, MuseBuffer &output,
                                    unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries) {
    m_copy_y_for_interpolation_algo->updateBufferDescriptorsInSet(descriptor_set_index,
                                                                  {frame.getVulkanBuffer(), output.getVulkanBuffer()});
    sq.enqueueComputeShader(
            m_copy_y_for_interpolation_algo,
            vector{field_parity, frame_phase_y, zero_non_copied_entries ? 1u : 0u},
            descriptor_set_index);
}

void Shaders::filterImageDiamond(CommandQueue &sq, int descriptor_set_index,
                                 int phase, MuseBuffer &buffer) {
    m_diamond_algo->updateBufferDescriptorsInSet(descriptor_set_index,
                                                 {m_diamond_filter_buffer.getVulkanBuffer(), buffer.getVulkanBuffer()});
    m_diamond_algo->updateWorkgroup(Workgroup(buffer.width(), buffer.height()));
    sq.enqueueComputeShader(
            m_diamond_algo,
            vector{m_diamond_filter_buffer.height(), m_diamond_filter_buffer.width(), buffer.height(), buffer.width(), (unsigned)phase},
            descriptor_set_index);
}

void Shaders::filterImage(CommandQueue &sq, int descriptor_set_index,
                          MuseBuffer &filter,
                          MuseBuffer &source, MuseBuffer &dest,
                          float border_value, float multiplier) {
    assert(source.height() == dest.height());
    assert(source.width() == dest.width());

    m_filter_image_algo->updateBufferDescriptorsInSet(
            descriptor_set_index,
            {filter.getVulkanBuffer(), source.getVulkanBuffer(), dest.getVulkanBuffer()});
    m_filter_image_algo->updateWorkgroup(Workgroup(source.width(), source.height()));
    sq.enqueueComputeShader(
            m_filter_image_algo,
            vector{(float)filter.height(), (float)filter.width(), (float)source.height(),
                        (float)source.width(), border_value, multiplier},
            descriptor_set_index);
}

void Shaders::decodeC(CommandQueue &sq, int descriptor_set_index,
                      MuseBuffer &input_frame,
                      int frame_phase_c, int field_parity, bool zero_non_sample_points) {
    m_decode_c_algo->updateBufferDescriptorsInSet(descriptor_set_index,
            {input_frame.getVulkanBuffer(), m_intermediate_r_buffer.getVulkanBuffer(), m_intermediate_b_buffer.getVulkanBuffer()});
    sq.enqueueComputeShader(
            m_decode_c_algo,
            vector{frame_phase_c, field_parity, zero_non_sample_points ? 1 : 0},
            descriptor_set_index);
}

// There are 5 fields in the vector.  Index 0 is the newest.
bool Shaders::decodeInterFrameAndDetectMotion(CommandQueue &sq, const vector<reference_wrapper<FieldBufferView>> &fields) {
    assert(fields.size() >= 5);
    if (!all_of(fields.cbegin(), fields.cend(),
                [](const reference_wrapper<FieldBufferView> f) -> bool {
                    return f.get().control_data().has_value() &&
                           f.get().control_data().value().field_subsampling_phase_Y.has_value() &&
                           f.get().control_data().value().frame_subsampling_phase_Y.has_value() &&
                           f.get().control_data().value().frame_subsampling_phase_C.has_value();
                })) {
        m_log.warn(eVideo, "Unknown phases for inter frame interpolation");
        return false;
    }

    vector<int> field_parities = vector<int> {
            fields[0].get().m_field_parity,
            fields[1].get().m_field_parity,
            fields[2].get().m_field_parity,
            fields[3].get().m_field_parity };
    vector<int> frame_phases_y = vector<int> {
            fields[0].get().control_data().value().frame_subsampling_phase_Y.value(),
            fields[1].get().control_data().value().frame_subsampling_phase_Y.value(),
            fields[2].get().control_data().value().frame_subsampling_phase_Y.value(),
            fields[3].get().control_data().value().frame_subsampling_phase_Y.value() };
    vector<int> field_phases_y = vector<int> {
            fields[0].get().control_data().value().field_subsampling_phase_Y.value(),
            fields[1].get().control_data().value().field_subsampling_phase_Y.value(),
            fields[2].get().control_data().value().field_subsampling_phase_Y.value(),
            fields[3].get().control_data().value().field_subsampling_phase_Y.value() };
    vector<int> frame_phases_c = vector<int> {
            fields[0].get().control_data().value().frame_subsampling_phase_C.value(),
            fields[1].get().control_data().value().frame_subsampling_phase_C.value(),
            fields[2].get().control_data().value().frame_subsampling_phase_C.value(),
            fields[3].get().control_data().value().frame_subsampling_phase_C.value() };

    if (!(field_parities[0] == field_parities[2] && field_parities[1] == field_parities[3] && field_parities[0] != field_parities[1]) ||
        !(field_phases_y[0] == field_phases_y[2] && field_phases_y[1] == field_phases_y[3] && field_phases_y[0] != field_phases_y[1]) ||
        !(frame_phases_y[0] != frame_phases_y[2] && frame_phases_y[1] != frame_phases_y[3])) {
        m_log.error(eVideo, "Inconsistent phases for inter frame interpolation");
        return false;
    }

    makeFieldFromConsecutiveFrames(sq, 1, fields[0], frame_phases_y[0], fields[2], frame_phases_y[2], field_parities[0],
                                   field_phases_y[0]);
    makeFieldFromConsecutiveFrames(sq, 3, fields[1], frame_phases_y[1], fields[3], frame_phases_y[3], field_parities[1],
                                   field_phases_y[1]);
    filterImageDiamond(sq, 1, field_parities[0] ^ field_phases_y[0], m_inter_frame_Y_buffer);

    for (int i = 0; i < 4; i++)
        decodeC(sq, 1 + i, fields[i].get().m_data, frame_phases_c[i], field_parities[i], i == 0);
    filterImage(sq, 2, m_color_filter_inter_frame_buffer, m_intermediate_r_buffer, m_inter_frame_r_buffer, 128.0 / 2, 2);
    filterImage(sq, 3, m_color_filter_inter_frame_buffer, m_intermediate_b_buffer, m_inter_frame_b_buffer, 128.0 / 2, 2);

    m_detect_motion_algo->updateBufferDescriptorsInSet(0, {
            fields[0].get().getVulkanBuffer(),
            fields[2].get().getVulkanBuffer(),
            fields[4].get().getVulkanBuffer(),
            m_movement_buffer.getVulkanBuffer(),
            m_movement_edge_buffer.getVulkanBuffer(),
            m_movement_coring_buffer.getVulkanBuffer()
    });
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity});

    return true;
}

void Shaders::makeFieldFromConsecutiveFrames(CommandQueue &sq,
                                             int copy_y_descriptor_set_first_index,
                                             FieldBufferView &field_a, unsigned int field_a_frame_phase_y,
                                             FieldBufferView &field_b, unsigned int field_b_frame_phase_y,
                                             unsigned int fields_parity, unsigned int fields_phases) {
    copyYForInterpolation(sq, copy_y_descriptor_set_first_index, field_a.m_data, m_interpolated32_buffer, fields_parity,
                          field_a_frame_phase_y, false);
    copyYForInterpolation(sq, copy_y_descriptor_set_first_index + 1, field_b.m_data, m_interpolated32_buffer,
                          fields_parity, field_b_frame_phase_y, false);

    sq.enqueueComputeShader(
            m_convert_sample_rate_4_to_3_algo,
            vector{m_filter_4_to_3_buffer->size(), 3u, 4u, 0u, 0u, m_interpolated32_buffer.height(),
                        m_interpolated32_buffer.width(), uint(1 - fields_parity), 2u, fields_phases, 2u, 2 * fields_phases, 1u});
}

void Shaders::combineStillAndMovingParts(CommandQueue &sq, bool force_field_only, bool force_inter_frame_only) {
    sq.enqueueComputeShader(m_combine_still_and_moving_algo,
                            vector{force_field_only ? 1u : 0u, force_inter_frame_only ? 1u : 0u});
}

shared_ptr<VulkanImage> Shaders::getResultImage() {
    return m_image_out;
}

void Shaders::convertAudioSampleRate(musevk::CommandQueue &sq, MuseBuffer &frame) {
    for (int i = 0; i < 2; i++) {
        m_convert_audio_sample_rate_algo->updateBufferDescriptorsInSet(
                i,
                {m_filter_4_to_3_buffer, frame.getVulkanBuffer(), m_audio_data.getVulkanBuffer()});
        sq.enqueueComputeShader(
                m_convert_audio_sample_rate_algo,
                vector{m_filter_4_to_3_buffer->size(), 3u, 4u,
                       2u + 562u * i, 2u, 44u, (uint32_t)MUSE_TOTAL_WIDTH,
                       44u * i, 1u, 0u, 1u, 0u, 1u},
                i);
    }
}

MuseBuffer &Shaders::getAudioData() {
    return m_audio_data;
}
