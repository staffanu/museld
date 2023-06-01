//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_COMPUTESHADER_H
#define MUSECPP_COMPUTESHADER_H

#include <vulkan/vulkan.hpp>
#include <memory>
#include "VulkanBuffer.h"

namespace musevk {
    typedef std::array<uint32_t, 3> Workgroup;

    class ComputeShader {
    public:
        template<typename S = float>
        ComputeShader(vk::Device &device,
                      const std::vector<std::shared_ptr<Tensor>> &tensors,
                      int32_t push_constants_size,
                      const std::vector<uint32_t> &spirv,
                      const Workgroup &workgroup)
                : m_device(device),
                  m_tensors(tensors),
                  m_spirv(spirv) {
            mWorkgroup = {workgroup[0],
                          workgroup[1] > 0 ? workgroup[1] : 1,
                          workgroup[2] > 0 ? workgroup[2] : 1};
            CreateParameters();
            CreateShaderModule();
            CreatePipeline(push_constants_size);
        }

        void recordBindCore(const vk::CommandBuffer &commandBuffer) {
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                             m_pipeline_layout,
                                             0, // First set
                                             m_descriptor_set,
                                             nullptr // Dispatcher
            );
        }

        template<typename T>
        void recordBindPush(const vk::CommandBuffer &commandBuffer, const std::vector<T> &push_constants) {
            if (!push_constants.empty()) {
                commandBuffer.pushConstants(m_pipeline_layout,
                                            vk::ShaderStageFlagBits::eCompute,
                                            0,
                                            sizeof(decltype(push_constants.back())) * push_constants.size(),
                                            push_constants.data());
            }
        }

        void recordDispatch(const vk::CommandBuffer &commandBuffer) {
            commandBuffer.dispatch(mWorkgroup[0], mWorkgroup[1], mWorkgroup[2]);
        }

        const std::vector<std::shared_ptr<Tensor>> &getTensors() {
            return m_tensors;
        }

        ~ComputeShader() {
            m_device.destroy(m_pipeline);
            m_device.destroy(m_pipeline_cache);
            m_device.destroy(m_pipeline_layout);
            m_device.destroy(m_shader_module);
            m_device.destroy(m_descriptor_set_layout);
            m_device.destroy(m_descriptorPool);
        }

    private:
        void CreateParameters() {
            std::vector<vk::DescriptorPoolSize> descriptorPoolSize = {
                    vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, m_tensors.size())
            };
            vk::DescriptorPoolCreateInfo descriptor_pool_info(
                    vk::DescriptorPoolCreateFlags(),
                    1, // Max sets
                    descriptorPoolSize);
            m_descriptorPool = m_device.createDescriptorPool(descriptor_pool_info);

            std::vector<vk::DescriptorSetLayoutBinding> descriptor_set_bindings;
            for (size_t i = 0; i < m_tensors.size(); i++) {
                descriptor_set_bindings.emplace_back(
                        i, // Binding index
                        vk::DescriptorType::eStorageBuffer,
                        1, // Descriptor count
                        vk::ShaderStageFlagBits::eCompute);
            }
            vk::DescriptorSetLayoutCreateInfo descriptor_set_layout_info(
                    vk::DescriptorSetLayoutCreateFlags(),
                    descriptor_set_bindings);
            m_descriptor_set_layout = m_device.createDescriptorSetLayout(descriptor_set_layout_info);

            vk::DescriptorSetAllocateInfo descriptor_set_allocate_info(m_descriptorPool, m_descriptor_set_layout);
            m_descriptor_set = m_device.allocateDescriptorSets(descriptor_set_allocate_info)[0];

            for (size_t i = 0; i < m_tensors.size(); i++) {
                std::vector<vk::WriteDescriptorSet> compute_write_descriptor_sets;

                vk::DescriptorBufferInfo descriptor_buffer_info =
                        m_tensors[i]->constructDescriptorBufferInfo();
                compute_write_descriptor_sets.emplace_back(
                        m_descriptor_set,
                        i, // Destination binding
                        0, // Destination array element
                        vk::DescriptorType::eStorageBuffer,
                        nullptr, // Descriptor image info
                        descriptor_buffer_info);

                m_device.updateDescriptorSets(compute_write_descriptor_sets, nullptr);
            }
        }

        void CreateShaderModule() {
            vk::ShaderModuleCreateInfo shaderModuleInfo(vk::ShaderModuleCreateFlags(),
                                                        sizeof(uint32_t) * m_spirv.size(),
                                                        m_spirv.data());
            m_shader_module = m_device.createShaderModule(shaderModuleInfo);
        }

        void CreatePipeline(int32_t push_constants_size) {
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

        vk::Device &m_device;
        std::vector<std::shared_ptr<Tensor>> m_tensors;

        vk::DescriptorSetLayout m_descriptor_set_layout;
        vk::DescriptorPool m_descriptorPool;
        vk::DescriptorSet m_descriptor_set;
        vk::ShaderModule m_shader_module;
        vk::PipelineLayout m_pipeline_layout;
        vk::PipelineCache m_pipeline_cache;
        vk::Pipeline m_pipeline;

        std::vector<uint32_t> m_spirv;
        Workgroup mWorkgroup;
    };
}

#endif //MUSECPP_COMPUTESHADER_H