// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <format>
#include "ComputeShader.h"

using namespace std;

namespace musevk {
    const Size ComputeShader::c_default_workgroup_size = Size(32, 2, 1);
    const Size ComputeShader::c_default_linear_workgroup_size = Size(1024, 1, 1);

    ComputeShader::ComputeShader(vk::Device &device,
                                 std::string name,
                                 const std::vector<MemoryObjectType> &buffer_types,
                                 int32_t push_constants_size,
                                 const std::vector<uint32_t> &spirv,
                                 const Size &workgroup_size,
                                 int max_descriptor_sets)
    : m_device(device),
      m_name(std::move(name)),
      m_descriptor_count(buffer_types.size()),
      m_push_constants_size(push_constants_size),
      m_spirv(spirv),
      m_workgroup_size(workgroup_size),
      m_local_workgroup_size(Size(0)) {

        initialize(buffer_types, max_descriptor_sets);
    }

    ComputeShader::ComputeShader(vk::Device &device,
                                 std::string name,
                                 const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
                                 int32_t push_constants_size,
                                 const std::vector<uint32_t> &spirv,
                                 const Size &workgroup_size,
                                 int max_descriptor_sets)
    : m_device(device),
      m_name(std::move(name)),
      m_descriptor_count(buffers.size()),
      m_push_constants_size(push_constants_size),
      m_spirv(spirv),
      m_workgroup_size(workgroup_size),
      m_local_workgroup_size(Size(0)) {

        std::vector<MemoryObjectType> buffer_types;
        buffer_types.reserve(buffers.size());
        for (auto &buffer : buffers)
            buffer_types.push_back(buffer->getType());

        initialize(buffer_types, max_descriptor_sets);

        updateBufferDescriptorsInSet(0, buffers);
    }

    void ComputeShader::initialize(const std::vector<MemoryObjectType> &buffer_types, int max_descriptor_sets) {

        m_local_workgroup_size = m_workgroup_size.y_size == 1 && m_workgroup_size.z_size == 1 ?
                c_default_linear_workgroup_size : c_default_workgroup_size;

        m_buffers.resize(max_descriptor_sets);
        createShaderModule();
        createDescriptorLayout(max_descriptor_sets, buffer_types);
        createPipeline();
        updateDescriptorSet(0);
    }

    ComputeShader::~ComputeShader() {
        m_device.destroy(m_pipeline);
        m_device.destroy(m_pipeline_cache);
        m_device.destroy(m_pipeline_layout);
        m_device.destroy(m_shader_module);
        m_device.destroy(m_descriptor_set_layout);
        m_device.destroy(m_descriptor_pool);
    }

    void ComputeShader::createShaderModule() {
        vk::ShaderModuleCreateInfo shaderModuleInfo(vk::ShaderModuleCreateFlags(),
                                                    sizeof(uint32_t) * m_spirv.size(),
                                                    m_spirv.data());
        m_shader_module = m_device.createShaderModule(shaderModuleInfo);
    }

    void ComputeShader::createDescriptorLayout(int number_of_descriptor_sets, const std::vector<MemoryObjectType> &buffer_types) {
        uint32_t number_of_storage_buffers = 0;
        uint32_t number_of_storage_images = 0;
        for (auto type : buffer_types) {
            switch (type) {
                case eBuffer:
                    number_of_storage_buffers++;
                    break;
                case eImage:
                    number_of_storage_images++;
                    break;
            }
        }
        vector<vk::DescriptorPoolSize> descriptor_pool_sizes;
        if (number_of_storage_buffers != 0)
            descriptor_pool_sizes.emplace_back(vk::DescriptorType::eStorageBuffer,
                                       number_of_storage_buffers * number_of_descriptor_sets);
        if (number_of_storage_images != 0)
            descriptor_pool_sizes.emplace_back(vk::DescriptorType::eStorageImage,
                                       number_of_storage_images * number_of_descriptor_sets);
        vk::DescriptorPoolCreateInfo descriptor_pool_info(
                vk::DescriptorPoolCreateFlags(),
                number_of_descriptor_sets,
                descriptor_pool_sizes);
        m_descriptor_pool = m_device.createDescriptorPool(descriptor_pool_info);

        vector<vk::DescriptorSetLayoutBinding> descriptor_set_bindings;
        for (size_t i = 0; i < m_descriptor_count; i++) {
            descriptor_set_bindings.emplace_back(
                    i, // Binding index
                    buffer_types[i] == eBuffer ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eStorageImage,
                    1, // Descriptor count
                    vk::ShaderStageFlagBits::eCompute);
        }
        vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info(
                vk::DescriptorSetLayoutCreateFlags(),
                descriptor_set_bindings);
        m_descriptor_set_layout = m_device.createDescriptorSetLayout(descriptor_set_layout_info);

        for (int i = 0; i < number_of_descriptor_sets; i++) {
            vk::DescriptorSetAllocateInfo descriptor_set_allocate_info(m_descriptor_pool, m_descriptor_set_layout);
            auto descriptor_set = m_device.allocateDescriptorSets(descriptor_set_allocate_info)[0];
            m_descriptor_sets.emplace_back(descriptor_set);
        }
    }

    void ComputeShader::updateDescriptorSet(int set_index) {
        auto buffers = m_buffers[set_index];
        for (size_t i = 0; i < buffers.size(); i++) {
            if (buffers[i] != nullptr) { // allow null to be updated later
                vector<vk::WriteDescriptorSet> compute_write_descriptor_sets;
                compute_write_descriptor_sets.push_back(
                        buffers[i]->makeWriteDescriptorSet(m_descriptor_sets[set_index], i));
                m_device.updateDescriptorSets(compute_write_descriptor_sets, nullptr);
            }
        }
    }

    void ComputeShader::createPipeline() {
        struct Constants {
            uint32_t size_x;
            uint32_t size_y;
            uint32_t size_z;
        } constants { m_local_workgroup_size.x_size, m_local_workgroup_size.y_size, m_local_workgroup_size.z_size };
        vector<vk::SpecializationMapEntry> specialization_map_entries {
                vk::SpecializationMapEntry(1, offsetof(Constants, size_x), sizeof(constants.size_x)),
                vk::SpecializationMapEntry(2, offsetof(Constants, size_y), sizeof(constants.size_y)),
                vk::SpecializationMapEntry(3, offsetof(Constants, size_z), sizeof(constants.size_z)),
        };
        vk::SpecializationInfo specialization_info(specialization_map_entries.size(),
                                                   specialization_map_entries.data(),
                                                   sizeof(constants),
                                                   &constants);

        vk::PipelineShaderStageCreateInfo shader_stage_info(vk::PipelineShaderStageCreateFlags(),
                                                            vk::ShaderStageFlagBits::eCompute,
                                                            m_shader_module,
                                                            "main",
                                                            &specialization_info);

        vk::PipelineCacheCreateInfo pipeline_cache_info = vk::PipelineCacheCreateInfo();
        m_pipeline_cache = m_device.createPipelineCache(pipeline_cache_info);

        vk::PipelineLayoutCreateInfo pipeline_layout_info(vk::PipelineLayoutCreateFlags(), m_descriptor_set_layout);
        vk::PushConstantRange pushConstantRange;
        if (m_push_constants_size) {
            pushConstantRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
            pushConstantRange.setOffset(0);
            pushConstantRange.setSize(m_push_constants_size);
            pipeline_layout_info.setPushConstantRangeCount(1);
            pipeline_layout_info.setPPushConstantRanges(&pushConstantRange);
        }
        m_pipeline_layout = m_device.createPipelineLayout(pipeline_layout_info);

        vk::ComputePipelineCreateInfo pipeline_info(vk::PipelineCreateFlags(),
                                                    shader_stage_info,
                                                    m_pipeline_layout,
                                                    vk::Pipeline(),
                                                    0);
        m_pipeline = m_device.createComputePipeline(m_pipeline_cache, pipeline_info).value;
    }

    void ComputeShader::bindPipelineAndDescriptorSets(const vk::CommandBuffer &commandBuffer, int descriptor_set_index) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                         m_pipeline_layout,
                                         0, // First set
                                         m_descriptor_sets[descriptor_set_index],
                                         nullptr // Dispatcher
        );
    }

    void ComputeShader::dispatch(const vk::CommandBuffer &commandBuffer, const vk::PhysicalDeviceLimits &limits) {
        uint32_t group_count_x = (m_workgroup_size.x_size + m_local_workgroup_size.x_size - 1) / m_local_workgroup_size.x_size;
        uint32_t group_count_y = (m_workgroup_size.y_size + m_local_workgroup_size.y_size - 1) / m_local_workgroup_size.y_size;
        uint32_t group_count_z = (m_workgroup_size.z_size + m_local_workgroup_size.z_size - 1) / m_local_workgroup_size.z_size;

        if (group_count_x > limits.maxComputeWorkGroupCount[0]
            || group_count_y > limits.maxComputeWorkGroupCount[1]
            || group_count_z > limits.maxComputeWorkGroupCount[2])
            throw std::runtime_error(std::format("Dispatch workgroup count too high: x: count={}, limit={}; y: count={}, limit={}; z: count={}, limit={}",
                                                 group_count_x, limits.maxComputeWorkGroupCount[0],
                                                 group_count_y, limits.maxComputeWorkGroupCount[1],
                                                 group_count_z, limits.maxComputeWorkGroupCount[2]));

//        if (group_count_x * group_count_y * group_count_z > limits.maxComputeWorkGroupInvocations)
//            throw std::runtime_error(std::format("Dispatch total count too high: count={}, limit={}",
//                                                 group_count_x * group_count_y * group_count_z, limits.maxComputeWorkGroupInvocations));

        commandBuffer.dispatch(group_count_x, group_count_y, group_count_z);
    }
}
