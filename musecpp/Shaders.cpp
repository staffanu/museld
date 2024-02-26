//
// Created by staffanu on 5/6/23.
//

#include <stdexcept>
#include <memory>
#include <fmt/format.h>
#include "MuseTypes.h"
#include "Shaders.h"
#include "FieldBufferView.h"
#include "musevk/VulkanManager.h"
#include "musevk/VulkanUtil.h"

using namespace std;
using namespace musevk;

Shaders::Shaders(Logger &log, std::string const &executable_dir, VulkanManager &manager)
: m_log(log),
  m_vulkan_manager(manager),
  m_non_linear_processed_buffer(createMuseBuffer(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH)),
  m_interpolated32_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_intermediate_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_Y_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_field_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_Y_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_inter_frame_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_current_movement_buffer_index(0),
  m_movement_buffers({ createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3),
                       createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3) }),
  m_movement_edge_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_coring_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_enlarged_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_image_out(make_unique<VulkanImage>(m_vulkan_manager.getMemoryAllocator(),
                                       m_vulkan_manager.getDevice(),
                                       MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
                                       vk::ImageLayout::eGeneral)),
  m_diamond_filter_buffer(m_vulkan_manager.createDeviceBufferFloatsAsHalfFloats(
          Size(9, 7),
          {
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
          })),
  m_color_filter_single_field_buffer(m_vulkan_manager.createDeviceBufferFloatsAsHalfFloats(
          Size(9, 5),
          {
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.008117, 0.021787, 0.054789, 0.087791, 0.101461, 0.087791, 0.054789, 0.021787, 0.008117,
                  0.004383, 0.011765, 0.029586, 0.047407, 0.054789, 0.047407, 0.029586, 0.011765, 0.004383,
                  0.000649, 0.001743, 0.004383, 0.007023, 0.008117, 0.007023, 0.004383, 0.001743, 0.000649,
          })),
  m_color_filter_inter_frame_buffer(m_vulkan_manager.createDeviceBufferFloatsAsHalfFloats(
          Size(5, 5),
          {
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.001980, 0.091620, 0.259228, 0.091620, 0.001980,
                  0.001069, 0.049475, 0.139983, 0.049475, 0.001069,
                  0.000158, 0.007330, 0.020738, 0.007330, 0.000158,
          })),
  m_filter_2_to_3_buffer(m_vulkan_manager.createDeviceBufferFloatsAsHalfFloats(
          Size(19),
          {
                  // cutoff 0.25, transition 0.05, sampling freq 1, Rectangular: 19 coeffs (25 non-zero).  Looks +/- 9 samples in each direction
                  0.034286805542972851, -0.000000000000000019, -0.044083035698107946, 0.000000000000000019,
                  0.061716249977351118, -0.000000000000000019, -0.102860416628918538,
                  0.000000000000000019, 0.308581249886755615, 0.484718293839893788, 0.308581249886755615,
                  0.000000000000000019, -0.102860416628918538, -0.000000000000000019,
                  0.061716249977351118, 0.000000000000000019, -0.044083035698107946, -0.000000000000000019,
                  0.034286805542972851
          })),
  m_filter_4_to_3_buffer(m_vulkan_manager.createDeviceBufferFloatsAsHalfFloats(
          Size(31),
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
    m_apply_eq_and_non_linear_spirv = VulkanUtil::loadSpirv(executable_dir, "apply_eq_and_non_linear.comp");
    m_apply_dropout_compensation_spirv = VulkanUtil::loadSpirv(executable_dir, "apply_dropout_compensation.comp");
    m_apply_deemphasis_and_gamma_spirv = VulkanUtil::loadSpirv(executable_dir, "apply_deemphasis_and_gamma.comp");
    m_diamond_spirv = VulkanUtil::loadSpirv(executable_dir, "filter_diamond.comp");
    m_filter_image_spirv = VulkanUtil::loadSpirv(executable_dir, "filter_image.comp");
    m_copy_y_for_interpolation_spirv = VulkanUtil::loadSpirv(executable_dir, "copy_y_for_interpolation.comp");
    m_convert_horiz_sample_rate_spirv = VulkanUtil::loadSpirv(executable_dir, "convert_horiz_sample_rate.comp");
    m_fill_empty_lines_spirv = VulkanUtil::loadSpirv(executable_dir, "fill_empty_lines.comp");
    m_decode_c_spirv = VulkanUtil::loadSpirv(executable_dir, "decode_c.comp");
    m_detect_motion_spirv = VulkanUtil::loadSpirv(executable_dir, "detect_motion.comp");
    m_combine_still_and_moving_spirv = VulkanUtil::loadSpirv(executable_dir, "combine_still_and_moving.comp");

    m_apply_eq_and_non_linear_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_eq_and_non_linear",
            {eBuffer, eBuffer}, sizeof(float) * 3,
            m_apply_eq_and_non_linear_spirv, Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_apply_dropout_compensation_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_dropout_compensation",
            {eBuffer, eBuffer}, 0,
            m_apply_dropout_compensation_spirv, Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_apply_deemphasis_and_gamma_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_deemphasis_and_gamma",
            {eBuffer, eBuffer}, 0,
            m_apply_deemphasis_and_gamma_spirv, Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_copy_y_for_interpolation_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "copy_y_for_interpolation",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            m_copy_y_for_interpolation_spirv, Size(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT), 8));
    m_diamond_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "diamond",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 5,
            m_diamond_spirv, Size(0), 2));
    m_filter_image_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "filter_image",
            {eBuffer, eBuffer, eBuffer}, sizeof(float) * 5,
            m_filter_image_spirv, Size(0), 4));
    m_fill_empty_lines_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "fill_empty_lines",
            {m_field_Y_buffer}, sizeof(uint32_t) * 3,
            m_fill_empty_lines_spirv, Size(m_field_Y_buffer->size().x_size, m_field_Y_buffer->size().y_size / 2)));
    m_convert_sample_rate_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_sample_rate",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Size(m_interpolated32_buffer->size().x_size / 4, m_interpolated32_buffer->size().y_size), 3));
    m_convert_sample_rate_4_to_3_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_sample_rate_4_to_3",
            {m_filter_4_to_3_buffer, m_interpolated32_buffer,
             m_inter_frame_Y_buffer}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Size(m_inter_frame_Y_buffer->size().x_size, m_inter_frame_Y_buffer->size().y_size / 2)));
    m_convert_sample_rate_2_to_3_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_sample_rate_2_to_3",
            {m_filter_2_to_3_buffer, m_interpolated32_buffer, m_field_Y_buffer},
            sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Size(m_field_Y_buffer->size().x_size, m_field_Y_buffer->size().y_size / 2)));
    m_decode_c_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "decode_c",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            m_decode_c_spirv, Size(m_intermediate_r_buffer->size().x_size, m_intermediate_r_buffer->size().y_size), 5));
    m_detect_motion_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "detect_motion",
            {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 2,
            m_detect_motion_spirv, Size(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT)));
    m_combine_still_and_moving_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "combine_still_and_moving",
            {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eImage},
            sizeof(uint32_t) * 2,
            m_combine_still_and_moving_spirv,
            Size(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2)));
    m_convert_audio_sample_rate_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_audio_sample_rate",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 13,
            m_convert_horiz_sample_rate_spirv,
            Size(MUSE_TOTAL_WIDTH, 44), 2));
}

std::shared_ptr<musevk::VulkanBuffer> Shaders::createMuseBuffer(unsigned int height, unsigned int width, bool host_visible) {
    return m_vulkan_manager.createBuffer(Size(width, height), 2 /* sizeof(float16) */, host_visible, false);
}

void Shaders::applyEqAndDeemphasisAndGamma(
        CommandBuffer &sq,
        shared_ptr<musevk::VulkanBuffer> const &video_input,
        shared_ptr<musevk::VulkanBuffer> const &dropout_input,
        shared_ptr<musevk::VulkanBuffer> const &buffer,
        pair<float, float> const &eq, bool enable_non_linear, bool enable_dropout_compensation) {
    m_apply_eq_and_non_linear_algo->updateBufferDescriptorsInSet(0, {video_input, m_non_linear_processed_buffer});
    sq.enqueueComputeShader(m_apply_eq_and_non_linear_algo,
                            {eq.first, eq.second, enable_non_linear ? 1.0f : 0.0f});

    if (enable_dropout_compensation) {
        m_apply_dropout_compensation_algo->
            updateBufferDescriptorsInSet(0, {m_non_linear_processed_buffer, dropout_input});
        sq.enqueueComputeShader(m_apply_dropout_compensation_algo, {});
    }

    m_apply_deemphasis_and_gamma_algo->updateBufferDescriptorsInSet(0, {m_non_linear_processed_buffer, buffer});
    sq.enqueueComputeShader(m_apply_deemphasis_and_gamma_algo, {});
}

void Shaders::decodeIntraField(CommandBuffer &sq, FieldBufferView &field) {
    m_log.info(eVideo, fmt::format("Decoding frame {} field {}", field.m_frame_no, field.m_field_parity));
    if (field.control_data().has_value())
        field.control_data().value().log_control_data();

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.control_data().has_value() ?
                        field.control_data().value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.control_data().has_value() ?
                        field.control_data().value().frame_subsampling_phase_C.value_or(0) : 0;
//    int field_phase = field.control_data().value().field_subsampling_phase_Y.value_or(0);

    copyYForInterpolation(sq, 0, field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y, true);
    filterImageDiamond(sq, 0, frame_phase_y, m_interpolated32_buffer);

    sq.enqueueComputeShader(
            m_convert_sample_rate_2_to_3_algo,
            vector{m_filter_2_to_3_buffer->size().x_size, 3u, 2u, 0u, 0u,
                   m_interpolated32_buffer->size().y_size, m_interpolated32_buffer->size().x_size,
                   uint(1 - field_parity), 2u, 0u, 1u, 0u, 1u});

    sq.enqueueComputeShader(m_fill_empty_lines_algo,
                            vector{m_field_Y_buffer->size().y_size, m_field_Y_buffer->size().x_size, (unsigned)field_parity});

    decodeC(sq, 0, field.m_data, frame_phase_c, field_parity, true);
    filterImage(sq, 0, m_color_filter_single_field_buffer, m_intermediate_r_buffer, m_field_r_buffer, 8);
    filterImage(sq, 1, m_color_filter_single_field_buffer, m_intermediate_b_buffer, m_field_b_buffer, 8);
}

void Shaders::copyYForInterpolation(CommandBuffer &sq, int descriptor_set_index,
                                    shared_ptr<VulkanBuffer> const &frame,
                                    shared_ptr<VulkanBuffer> const &output,
                                    unsigned int field_parity, unsigned int frame_phase_y, bool zero_non_copied_entries) {
    m_copy_y_for_interpolation_algo->updateBufferDescriptorsInSet(descriptor_set_index, {frame, output});
    sq.enqueueComputeShader(
            m_copy_y_for_interpolation_algo,
            vector{field_parity, frame_phase_y, zero_non_copied_entries ? 1u : 0u},
            descriptor_set_index);
}

void Shaders::filterImageDiamond(CommandBuffer &sq, int descriptor_set_index,
                                 int phase, shared_ptr<VulkanBuffer> const &buffer) {
    m_diamond_algo->updateBufferDescriptorsInSet(descriptor_set_index, {m_diamond_filter_buffer, buffer});
    m_diamond_algo->updateWorkgroup(buffer->size());
    sq.enqueueComputeShader(
            m_diamond_algo,
            vector{m_diamond_filter_buffer->size().y_size, m_diamond_filter_buffer->size().x_size,
                   buffer->size().y_size, buffer->size().x_size, (unsigned)phase},
            descriptor_set_index);
}

void Shaders::filterImage(CommandBuffer &sq, int descriptor_set_index,
                          shared_ptr<musevk::VulkanBuffer> const &filter,
                          shared_ptr<musevk::VulkanBuffer> const &source,
                          shared_ptr<musevk::VulkanBuffer> const &dest,
                          float multiplier) {
    assert(source->size() == dest->size());

    m_filter_image_algo->updateBufferDescriptorsInSet(
            descriptor_set_index,
            {filter, source, dest});
    m_filter_image_algo->updateWorkgroup(source->size());
    sq.enqueueComputeShader(
            m_filter_image_algo,
            vector{(float)filter->size().y_size, (float)filter->size().x_size, (float)source->size().y_size,
                        (float)source->size().x_size, multiplier},
            descriptor_set_index);
}

void Shaders::decodeC(CommandBuffer &sq, int descriptor_set_index,
                      shared_ptr<VulkanBuffer> const &input_frame,
                      int frame_phase_c, int field_parity, bool zero_non_sample_points) {
    m_decode_c_algo->updateBufferDescriptorsInSet(descriptor_set_index,
            {input_frame, m_intermediate_r_buffer, m_intermediate_b_buffer});
    sq.enqueueComputeShader(
            m_decode_c_algo,
            vector{frame_phase_c, field_parity, zero_non_sample_points ? 1 : 0},
            descriptor_set_index);
}

// There are 5 fields in the vector.  Index 0 is the newest.
bool Shaders::decodeInterFrameAndDetectMotion(CommandBuffer &sq,
                                              const vector<reference_wrapper<FieldBufferView>> &fields,
                                              bool use_prev_motion_info) {
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
    filterImage(sq, 2, m_color_filter_inter_frame_buffer, m_intermediate_r_buffer, m_inter_frame_r_buffer, 2);
    filterImage(sq, 3, m_color_filter_inter_frame_buffer, m_intermediate_b_buffer, m_inter_frame_b_buffer, 2);

    m_current_movement_buffer_index = 1 - m_current_movement_buffer_index;
    m_detect_motion_algo->updateBufferDescriptorsInSet(0, {
            fields[0].get().getVulkanBuffer(),
            fields[2].get().getVulkanBuffer(),
            fields[4].get().getVulkanBuffer(),
            m_movement_buffers[1 - m_current_movement_buffer_index],
            m_movement_buffers[m_current_movement_buffer_index],
            m_movement_edge_buffer,
            m_movement_coring_buffer,
            m_movement_enlarged_buffer,
    });
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity, use_prev_motion_info ? 1 : 0});

    return true;
}

void Shaders::makeFieldFromConsecutiveFrames(CommandBuffer &sq,
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
            vector{m_filter_4_to_3_buffer->size().x_size, 3u, 4u, 0u, 0u, m_interpolated32_buffer->size().y_size,
                        m_interpolated32_buffer->size().x_size, uint(1 - fields_parity), 2u, fields_phases, 2u, 2 * fields_phases, 1u});
}

void Shaders::combineStillAndMovingParts(CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only) {
    m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eGeneral,
                                         vk::PipelineStageFlagBits::eTopOfPipe,
                                         vk::PipelineStageFlagBits::eComputeShader,
                                         vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite);
    m_combine_still_and_moving_algo->updateBufferDescriptorsInSet(
            0,
            {m_field_Y_buffer, m_field_r_buffer,
             m_field_b_buffer, m_inter_frame_Y_buffer,
             m_inter_frame_r_buffer, m_inter_frame_b_buffer,
             m_movement_buffers[m_current_movement_buffer_index], m_image_out});
    sq.enqueueComputeShader(m_combine_still_and_moving_algo,
                            vector{force_field_only ? 1u : 0u, force_inter_frame_only ? 1u : 0u});
}

shared_ptr<VulkanImage> Shaders::getResultImage() {
    return m_image_out;
}

void Shaders::convertAudioSampleRate(musevk::CommandBuffer &sq, shared_ptr<VulkanBuffer> const &frame) {
    for (int i = 0; i < 2; i++) {
        m_convert_audio_sample_rate_algo->updateBufferDescriptorsInSet(
                i,
                {m_filter_4_to_3_buffer, frame, m_audio_data});
        sq.enqueueComputeShader(
                m_convert_audio_sample_rate_algo,
                vector{m_filter_4_to_3_buffer->size().x_size, 3u, 4u,
                       2u + 562u * i, 2u, 44u, (uint32_t)MUSE_TOTAL_WIDTH,
                       44u * i, 1u, 0u, 1u, 0u, 1u},
                i);
    }
}

shared_ptr<VulkanBuffer> Shaders::getAudioData() const {
    return m_audio_data;
}
