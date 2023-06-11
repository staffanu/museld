//
// Created by staffanu on 5/31/23.
//

#include "ComputeShader.h"

namespace musevk {
    ComputeShader::ComputeShader(std::string &name,
                                 vk::Device &device,
                                 const std::vector<MemoryObjectType> &buffer_types,
                                 int32_t push_constants_size,
                                 const std::vector<uint32_t> &spirv,
                                 const Workgroup &workgroup,
                                 int max_descriptor_sets)
: m_name(name),
  m_device(device),
  m_descriptor_count(buffer_types.size()),
  m_spirv(spirv),
  m_workgroup(workgroup) {
        m_buffers.resize(max_descriptor_sets);
        createShaderModule();
        createDescriptorLayout(max_descriptor_sets, buffer_types);
        createPipeline(push_constants_size);
        updateDescriptorSet(0);
    }

    ComputeShader::ComputeShader(std::string &name,
                                 vk::Device &device,
                                 const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
                                 int32_t push_constants_size,
                                 const std::vector<uint32_t> &spirv,
                                 const Workgroup &workgroup,
                                 int max_descriptor_sets)
: m_name(name),
  m_device(device),
  m_descriptor_count(buffers.size()),
  m_spirv(spirv),
  m_workgroup(workgroup)
    {
        std::vector<MemoryObjectType> buffer_types;
        buffer_types.reserve(buffers.size());
        for (auto &buffer : buffers)
            buffer_types.push_back(buffer->getType());

        m_buffers.resize(max_descriptor_sets);
        createShaderModule();
        createDescriptorLayout(max_descriptor_sets, buffer_types);
        createPipeline(push_constants_size);
        updateDescriptorSet(0);

        updateBufferDescriptorsInSet(0, buffers);
    }

    void ComputeShader::createShaderModule() {
        vk::ShaderModuleCreateInfo shaderModuleInfo(vk::ShaderModuleCreateFlags(),
                                                    sizeof(uint32_t) * m_spirv.size(),
                                                    m_spirv.data());
        m_shader_module = m_device.createShaderModule(shaderModuleInfo);
    }

    void ComputeShader::createDescriptorLayout(int number_of_descriptor_sets, const std::vector<MemoryObjectType> &buffer_types) {
        std::vector<vk::DescriptorPoolSize> descriptor_pool_size = {
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,
                                       m_descriptor_count * number_of_descriptor_sets)
        };
        vk::DescriptorPoolCreateInfo descriptor_pool_info(
                vk::DescriptorPoolCreateFlags(),
                number_of_descriptor_sets,
                descriptor_pool_size);
        m_descriptor_pool = m_device.createDescriptorPool(descriptor_pool_info);

        std::vector<vk::DescriptorSetLayoutBinding> descriptor_set_bindings;
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
                std::vector<vk::WriteDescriptorSet> compute_write_descriptor_sets;
                compute_write_descriptor_sets.push_back(
                        buffers[i]->makeWriteDescriptorSet(m_descriptor_sets[set_index], i));
                m_device.updateDescriptorSets(compute_write_descriptor_sets, nullptr);
            }
        }
    }

    void ComputeShader::createPipeline(int32_t push_constants_size) {
        vk::PipelineShaderStageCreateInfo shader_stage_info(vk::PipelineShaderStageCreateFlags(),
                                                            vk::ShaderStageFlagBits::eCompute,
                                                            m_shader_module,
                                                            "main",
                                                            nullptr);

        vk::PipelineCacheCreateInfo pipeline_cache_info = vk::PipelineCacheCreateInfo();
        m_pipeline_cache = m_device.createPipelineCache(pipeline_cache_info);

        vk::PipelineLayoutCreateInfo pipeline_layout_info(vk::PipelineLayoutCreateFlags(), m_descriptor_set_layout);
        vk::PushConstantRange pushConstantRange;
        if (push_constants_size) {
            pushConstantRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
            pushConstantRange.setOffset(0);
            pushConstantRange.setSize(push_constants_size);
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
}
