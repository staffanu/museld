//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_COMPUTESHADER_H
#define MUSECPP_COMPUTESHADER_H

#include <vulkan/vulkan.hpp>
#include "VulkanBuffer.h"

namespace musevk {
    struct Workgroup {
        Workgroup(uint32_t x, uint32_t y = 1, uint32_t z = 1)
        : x_size(x),
          y_size(y),
          z_size(z) {};
        uint32_t x_size;
        uint32_t y_size;
        uint32_t z_size;
    };

    class ComputeShader {
    public:
        ComputeShader(std::string &name,
                      vk::Device &device,
                      const std::vector<MemoryObjectType> &buffer_types,
                      int32_t push_constants_size,
                      const std::vector<uint32_t> &spirv,
                      const Workgroup &workgroup,
                      int max_descriptor_sets);

        ComputeShader(std::string &name,
                      vk::Device &device,
                      const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
                      int32_t push_constants_size,
                      const std::vector<uint32_t> &spirv,
                      const Workgroup &workgroup,
                      int max_descriptor_sets);

        std::string &name() {
            return m_name;
        }

        // A descriptor set can be used only once for a single command queue submit.  If we
        // re-use the same shader for several inputs, we create multiple descriptor sets.
        // They all have to conform to the same layout.
        void updateBufferDescriptorsInSet(int set_index, const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers) {
            assert(buffers.size() == m_descriptor_count);
            m_buffers[set_index] = buffers;
            updateDescriptorSet(set_index);
        }

        void updateWorkgroup(Workgroup const &workgroup) {
            m_workgroup = workgroup;
        }

        const std::vector<std::shared_ptr<VulkanMemoryObject>> &getMemoryObjects(int descriptor_set_index) {
            return m_buffers[descriptor_set_index];
        }

        ~ComputeShader() {
            m_device.destroy(m_pipeline);
            m_device.destroy(m_pipeline_cache);
            m_device.destroy(m_pipeline_layout);
            m_device.destroy(m_shader_module);
            m_device.destroy(m_descriptor_set_layout);
            m_device.destroy(m_descriptor_pool);
        }

    private:
        friend class CommandQueue;

        void createShaderModule();
        void createDescriptorLayout(int number_of_descriptor_sets, const std::vector<MemoryObjectType> &buffer_types);
        void updateDescriptorSet(int set_index);
        void createPipeline(int32_t push_constants_size);
        void bindPipelineAndDescriptorSets(const vk::CommandBuffer &commandBuffer, int descriptor_set_index) {
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                             m_pipeline_layout,
                                             0, // First set
                                             m_descriptor_sets[descriptor_set_index],
                                             nullptr // Dispatcher
            );
        }

        template<typename T>
        void bindPushConstants(const vk::CommandBuffer &commandBuffer, const std::vector<T> &push_constants) {
            if (!push_constants.empty()) {
                commandBuffer.pushConstants(m_pipeline_layout,
                                            vk::ShaderStageFlagBits::eCompute,
                                            0,
                                            sizeof(T) * push_constants.size(),
                                            push_constants.data());
            }
        }

        void dispatch(const vk::CommandBuffer &commandBuffer) {
            commandBuffer.dispatch(m_workgroup.x_size, m_workgroup.y_size, m_workgroup.z_size);
        }

        std::string m_name;
        vk::Device &m_device;
        size_t m_descriptor_count;
        std::vector<std::vector<std::shared_ptr<VulkanMemoryObject>>> m_buffers;

        vk::DescriptorSetLayout m_descriptor_set_layout;
        vk::DescriptorPool m_descriptor_pool;
        std::vector<vk::DescriptorSet> m_descriptor_sets;
        vk::ShaderModule m_shader_module;
        vk::PipelineLayout m_pipeline_layout;
        vk::PipelineCache m_pipeline_cache;
        vk::Pipeline m_pipeline;

        std::vector<uint32_t> m_spirv;
        Workgroup m_workgroup;
    };
}

#endif //MUSECPP_COMPUTESHADER_H