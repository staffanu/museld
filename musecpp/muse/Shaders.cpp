//
// Created by staffanu on 5/6/23.
//

#include <stdexcept>
#include <memory>
#include "MuseConstants.h"
#include "Shaders.h"
#include "FieldBufferView.h"
#include "musevk/VulkanManager.h"
#include "musevk/VulkanUtil.h"

using namespace std;
using namespace musevk;

Shaders::Shaders(Logger &log, std::string const &executable_dir, VulkanManager &manager, CommandPool &command_pool)
: m_log(log),
  m_vulkan_manager(manager),
  m_command_pool(command_pool),
  m_non_linear_processed_buffer(createMuseBuffer(MUSE_TOTAL_HEIGHT, MUSE_TOTAL_WIDTH)),
  m_interpolated32_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH * 2)),
  m_intermediate_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH)),
  m_intermediate_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH)),
  m_field_Y_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_field_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT / 2, MUSE_Y_BUF_WIDTH / 2)),
  m_field_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT / 2, MUSE_Y_BUF_WIDTH / 2)),
  m_inter_frame_Y_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3)),
  m_inter_frame_r_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH)),
  m_inter_frame_b_buffer(createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH)),
  m_current_movement_buffer_index(0),
  m_movement_buffers({ createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3),
                       createMuseBuffer(MUSE_BUF_HEIGHT * 2, MUSE_Y_BUF_WIDTH * 3) }),
  m_movement_edge_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_coring_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_movement_enlarged_buffer(createMuseBuffer(MUSE_BUF_HEIGHT, MUSE_Y_BUF_WIDTH)),
  m_image_out(make_unique<VulkanImage>(m_vulkan_manager,
                                       MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc, eHostNone)),
  m_image_Y_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_U_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(MUSE_Y_BUF_WIDTH * 3 / 2, MUSE_BUF_HEIGHT * 2 / 2), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_image_V_out(make_unique<VulkanBuffer>(m_vulkan_manager, Size(MUSE_Y_BUF_WIDTH * 3 / 2, MUSE_BUF_HEIGHT * 2 / 2), 2,
                                          vk::BufferUsageFlagBits::eStorageBuffer, eHostRead)),
  m_diamond_filter_buffer(VulkanUtil::createDeviceBufferFloatsAsHalfFloats(m_vulkan_manager, m_command_pool,
          Size(9, 7), // Notice the total size should not be larger than the workgroup size!
          { // Notice we really only use half of the coefficients, so this could be made smaller!
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.001410, 0.006845, 0.006383, 0.146909, 0.398459, 0.146909, 0.006383, 0.006845, 0.001410,
                  -0.000724, -0.002093, -0.022435, 0.024072, 0.164560, 0.024072, -0.022435, -0.002093, -0.000724,
                  0.000205, 0.000474, -0.005036, -0.012591, 0.010491, -0.012591, -0.005036, 0.000474, 0.000205,
                  -0.000096, 0.000300, 0.001529, -0.001499, -0.000041, -0.001499, 0.001529, 0.000300, -0.000096,
          })),
  m_filter_2_to_3_buffer(VulkanUtil::createDeviceBufferFloatsAsHalfFloats(m_vulkan_manager, m_command_pool,
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
  m_filter_4_to_3_buffer(VulkanUtil::createDeviceBufferFloatsAsHalfFloats(m_vulkan_manager, m_command_pool,
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
  m_audio_data(createMuseBuffer(88, MUSE_TOTAL_WIDTH * 3 / 4, eHostRead))
{
    m_apply_eq_and_non_linear_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_eq_and_non_linear",
            {eBuffer, eBuffer}, sizeof(float) * 3,
            VulkanUtil::loadSpirv(executable_dir, "apply_eq_and_non_linear.comp"), Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_apply_dropout_compensation_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_dropout_compensation",
            {eBuffer, eBuffer}, 4,
            VulkanUtil::loadSpirv(executable_dir, "apply_dropout_compensation.comp"), Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_apply_deemphasis_and_gamma_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "apply_deemphasis_and_gamma",
            {eBuffer, eBuffer}, 0,
            VulkanUtil::loadSpirv(executable_dir, "apply_deemphasis_and_gamma.comp"), Size(MUSE_TOTAL_WIDTH, MUSE_TOTAL_HEIGHT)));
    m_copy_y_for_interpolation_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "copy_y_for_interpolation",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            VulkanUtil::loadSpirv(executable_dir, "copy_y_for_interpolation.comp"), Size(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT), 8));
    m_diamond_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "diamond",
            {eBuffer, eBuffer}, sizeof(uint32_t) * 5,
            VulkanUtil::loadSpirv(executable_dir, "filter_diamond.comp"), Size(0), 4));
    m_fill_empty_lines_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "fill_empty_lines",
            {m_field_Y_buffer}, sizeof(uint32_t) * 3,
            VulkanUtil::loadSpirv(executable_dir, "fill_empty_lines.comp"),
            Size(m_field_Y_buffer->size().x_size, m_field_Y_buffer->size().y_size / 2)));
    m_convert_sample_rate_4_to_3_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_sample_rate_4_to_3",
            {m_filter_4_to_3_buffer, m_interpolated32_buffer,
             m_inter_frame_Y_buffer}, sizeof(uint32_t) * 12,
             VulkanUtil::loadSpirv(executable_dir, "convert_horiz_sample_rate.comp"),
            Size(m_inter_frame_Y_buffer->size().x_size, m_inter_frame_Y_buffer->size().y_size / 2)));
    m_convert_sample_rate_2_to_3_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_sample_rate_2_to_3",
            {m_filter_2_to_3_buffer, m_interpolated32_buffer, m_field_Y_buffer},
            sizeof(uint32_t) * 12,
            VulkanUtil::loadSpirv(executable_dir, "convert_horiz_sample_rate.comp"),
            Size(m_field_Y_buffer->size().x_size, m_field_Y_buffer->size().y_size / 2)));
    m_decode_c_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
                                                                  "decode_c",
                                                                  {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
                                                                  VulkanUtil::loadSpirv(executable_dir, "decode_c.comp"),
                                                                  Size(MUSE_C_BUF_WIDTH, MUSE_BUF_HEIGHT * 2), 4));
    m_filter_c_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
                                                                  "filter_c",
                                                                  {eBuffer}, sizeof(uint32_t) * 2,
                                                                  VulkanUtil::loadSpirv(executable_dir, "filter_c.comp"),
                                                                  Size(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT), 2));
    m_decode_c2_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
                                                                  "decode_c2",
                                                                  {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 2,
                                                                  VulkanUtil::loadSpirv(executable_dir, "decode_c2.comp"),
                                                                  Size(MUSE_C_BUF_WIDTH, MUSE_BUF_HEIGHT / 2), 5));
//    Size(m_intermediate_r_buffer->size().x_size, m_intermediate_r_buffer->size().y_size), 5));
    m_detect_motion_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "detect_motion",
            {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 3,
            VulkanUtil::loadSpirv(executable_dir, "detect_motion.comp"), Size(MUSE_Y_BUF_WIDTH, MUSE_BUF_HEIGHT)));
    m_combine_still_and_moving_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "combine_still_and_moving",
            {eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eBuffer, eImage, eBuffer, eBuffer, eBuffer},
            sizeof(uint32_t) * 3,
            VulkanUtil::loadSpirv(executable_dir, "combine_still_and_moving.comp"),
            Size(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2)));
    m_convert_audio_sample_rate_algo = shared_ptr<ComputeShader>(new ComputeShader(m_vulkan_manager.getDevice(),
            "convert_audio_sample_rate",
            {eBuffer, eBuffer, eBuffer}, sizeof(uint32_t) * 13,
            VulkanUtil::loadSpirv(executable_dir, "convert_horiz_sample_rate.comp"),
            Size(MUSE_TOTAL_WIDTH, 44), 2));
}

std::shared_ptr<musevk::VulkanBuffer> Shaders::createMuseBuffer(unsigned int height, unsigned int width, HostAccess host_access) {
    return make_unique<VulkanBuffer>(m_vulkan_manager, Size(width, height), 2 /* sizeof(float16) */, vk::BufferUsageFlagBits::eStorageBuffer, host_access);
}

void Shaders::applyEqAndDeemphasisAndGamma(
        CommandBuffer &sq,
        shared_ptr<musevk::VulkanBuffer> const &video_input,
        shared_ptr<musevk::VulkanBuffer> const &dropout_input,
        shared_ptr<musevk::VulkanBuffer> const &buffer,
        pair<float, float> const &eq, bool enable_non_linear,  DropoutMode dropout_mode) {
    m_apply_eq_and_non_linear_algo->updateBufferDescriptorsInSet(0, {video_input, m_non_linear_processed_buffer});
    sq.enqueueComputeShader<float>(m_apply_eq_and_non_linear_algo,
                            {eq.first, eq.second, enable_non_linear ? 1.0f : 0.0f});

    if (dropout_mode != DropoutMode::eDisabled) {
        m_apply_dropout_compensation_algo->
            updateBufferDescriptorsInSet(0, {m_non_linear_processed_buffer, dropout_input});
        sq.enqueueComputeShader<uint32_t>(m_apply_dropout_compensation_algo, { dropout_mode == DropoutMode::eHighlight ? 2u : 0u });
    }

    m_apply_deemphasis_and_gamma_algo->updateBufferDescriptorsInSet(0, {m_non_linear_processed_buffer, buffer});
    sq.enqueueComputeShader<float>(m_apply_deemphasis_and_gamma_algo, {});
}

void Shaders::decodeIntraField(CommandBuffer &sq, FieldBufferView &field) {
    if (field.control_data())
        field.control_data().value().log_control_data();

    int field_parity = field.m_field_parity;
    int frame_phase_y = field.control_data() ?
                        field.control_data().value().frame_subsampling_phase_Y.value_or(0) : 0;
    int frame_phase_c = field.control_data() ?
                        field.control_data().value().frame_subsampling_phase_C.value_or(0) : 0;
//    int field_phase = field.control_data().value().field_subsampling_phase_Y.value_or(0);

    copyYForInterpolation(sq, 0, field.m_data, m_interpolated32_buffer, field_parity, frame_phase_y, true);
    filterImageDiamond(sq, 0, frame_phase_y, m_interpolated32_buffer);

    sq.enqueueComputeShader(
            m_convert_sample_rate_2_to_3_algo,
            vector{m_filter_2_to_3_buffer->size().x_size, 3u, 2u, 0u, 0u,
                   m_interpolated32_buffer->size().y_size, m_interpolated32_buffer->size().x_size,
                   uint(1 - field_parity), 2u, 0u, 1u, 0u});

    sq.enqueueComputeShader(m_fill_empty_lines_algo,
                            vector{m_field_Y_buffer->size().y_size, m_field_Y_buffer->size().x_size, (unsigned)field_parity});

    decodeC2(sq, 0, field.m_data, frame_phase_c, field_parity);
    filterImageDiamond(sq, 2, frame_phase_c ^ field_parity, m_field_r_buffer);
    filterImageDiamond(sq, 3, 1 - frame_phase_c ^ field_parity, m_field_b_buffer);
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
    m_diamond_algo->updateWorkgroup(Size(buffer->size().x_size / 2, buffer->size().y_size));
    sq.enqueueComputeShader(
            m_diamond_algo,
            vector{m_diamond_filter_buffer->size().y_size, m_diamond_filter_buffer->size().x_size,
                   buffer->size().y_size, buffer->size().x_size, (unsigned)phase},
            descriptor_set_index);
}

void Shaders::decodeC(CommandBuffer &sq, int descriptor_set_index,
                      shared_ptr<VulkanBuffer> const &input_frame,
                      int frame_phase_c, int field_parity, bool clear_output) {
    m_decode_c_algo->updateBufferDescriptorsInSet(descriptor_set_index,
                                                  {input_frame, m_inter_frame_r_buffer, m_inter_frame_b_buffer});
    sq.enqueueComputeShader(
            m_decode_c_algo,
            vector{frame_phase_c, field_parity, clear_output ? 1 : 0},
            descriptor_set_index);
}

void Shaders::decodeC2(CommandBuffer &sq, int descriptor_set_index,
                      shared_ptr<VulkanBuffer> const &input_frame,
                      int frame_phase_c, int field_parity) {
    m_decode_c2_algo->updateBufferDescriptorsInSet(descriptor_set_index,
                                                  {input_frame, m_field_r_buffer, m_field_b_buffer});
    sq.enqueueComputeShader(
            m_decode_c2_algo,
            vector{frame_phase_c, field_parity},
            descriptor_set_index);
}

// There are 5 fields in the vector.  Index 0 is the newest.
bool Shaders::decodeInterFrameAndDetectMotion(CommandBuffer &sq,
                                              const vector<reference_wrapper<FieldBufferView>> &fields,
                                              bool use_prev_motion_info) {
    assert(fields.size() >= 5);
    if (!all_of(fields.cbegin(), fields.cend(),
                [](const reference_wrapper<FieldBufferView> f) -> bool {
                    return f.get().control_data() &&
                           f.get().control_data().value().field_subsampling_phase_Y &&
                           f.get().control_data().value().frame_subsampling_phase_Y &&
                           f.get().control_data().value().frame_subsampling_phase_C;
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
        decodeC(sq, i, fields[i].get().m_data, frame_phases_c[i], field_parities[i], i == 0);

    m_filter_c_algo->updateBufferDescriptorsInSet(0, {m_inter_frame_r_buffer});
    sq.enqueueComputeShader(m_filter_c_algo, vector{1u /* is_red */, 0u /* algo stage */}, 0);
    sq.enqueueComputeShader(m_filter_c_algo, vector{1u /* is_red */, 1u /* algo stage */}, 0);
    m_filter_c_algo->updateBufferDescriptorsInSet(1, {m_inter_frame_b_buffer});
    sq.enqueueComputeShader(m_filter_c_algo, vector{0u /* is_red */, 0u /* algo stage */}, 1);
    sq.enqueueComputeShader(m_filter_c_algo, vector{0u /* is_red */, 1u /* algo stage */}, 1);

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
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity, use_prev_motion_info ? 1 : 0, 1});
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity, use_prev_motion_info ? 1 : 0, 2});
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity, use_prev_motion_info ? 1 : 0, 3});
    sq.enqueueComputeShader(m_detect_motion_algo, vector{fields[0].get().m_field_parity, use_prev_motion_info ? 1 : 0, 4});

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
                        m_interpolated32_buffer->size().x_size, uint(1 - fields_parity), 2u, fields_phases, 2u, 2 * fields_phases});
}

void Shaders::combineStillAndMovingParts(CommandBuffer &sq, bool force_field_only, bool force_inter_frame_only, bool output_yuv) {
    m_image_out->enqueueTransitionLayout(sq, vk::ImageLayout::eGeneral,
                                         vk::PipelineStageFlagBits::eTopOfPipe,
                                         vk::PipelineStageFlagBits::eComputeShader,
                                         vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite);
    m_combine_still_and_moving_algo->updateBufferDescriptorsInSet(
            0,
            {m_field_Y_buffer, m_field_r_buffer,
             m_field_b_buffer, m_inter_frame_Y_buffer,
             m_inter_frame_r_buffer, m_inter_frame_b_buffer,
             m_movement_buffers[m_current_movement_buffer_index], m_image_out,
             m_image_Y_out, m_image_U_out, m_image_V_out});
    sq.enqueueComputeShader(m_combine_still_and_moving_algo,
                            vector{force_field_only ? 1u : 0u, force_inter_frame_only ? 1u : 0u, output_yuv ? 1u : 0u});

    if (output_yuv) {
        m_image_Y_out->synchronizeForHostRead(sq);
        m_image_U_out->synchronizeForHostRead(sq);
        m_image_V_out->synchronizeForHostRead(sq);
    }
}

Shaders::ResultImages Shaders::getResultImages() {
    return ResultImages { m_image_out, m_image_Y_out, m_image_V_out, m_image_U_out};
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
    m_audio_data->synchronizeForHostRead(sq);
}

shared_ptr<VulkanBuffer> Shaders::getAudioData() const {
    return m_audio_data;
}
