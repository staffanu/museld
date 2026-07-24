// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_COMPUTESHADER_H
#define MUSECPP_COMPUTESHADER_H

#include <vulkan/vulkan.hpp>
#include "VulkanMemoryObject.h"
#include "Size.h"

namespace musevk {
    // VulkanManager.h reaches this header through CommandBuffer.h, so it can only be named here.
    class VulkanManager;

    class ComputeShader {
    public:
        ComputeShader(VulkanManager &vulkan_manager,
                      std::string name,
                      const std::vector<MemoryObjectType> &buffer_types,
                      int32_t push_constants_size,
                      const std::vector<uint32_t> &spirv,
                      const Size &workgroup_size,
                      int max_descriptor_sets = 1,
                      const std::vector<uint32_t> &specialization_constants = {});

        ComputeShader(VulkanManager &vulkan_manager,
                      std::string name,
                      const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
                      int32_t push_constants_size,
                      const std::vector<uint32_t> &spirv,
                      const Size &workgroup_size,
                      int max_descriptor_sets = 1,
                      const std::vector<uint32_t> &specialization_constants = {});

        ~ComputeShader();

        std::string &name() {
            return m_name;
        }

        // A descriptor set can be used only once for a single command queue submit.  If we
        // re-use the same shader for several inputs, we create multiple descriptor sets.
        // They all have to conform to the same layout.
        void updateBufferDescriptorsInSet(int set_index, const std::vector<std::shared_ptr<VulkanMemoryObject>> buffers) {
            assert(buffers.size() == m_descriptor_count);
            m_buffers[set_index] = buffers;
            updateDescriptorSet(set_index);
        }

        // Can be used to check if the descriptors need to be updated when calling the same shader several times
        std::vector<std::shared_ptr<VulkanMemoryObject>> getBuffersForDescriptorSet(int index) {
            return m_buffers[index];
        }

        void updateWorkgroup(Size const &workgroup) {
            m_workgroup_size = workgroup;
        }

        const std::vector<std::shared_ptr<VulkanMemoryObject>> &getMemoryObjects(int descriptor_set_index) {
            return m_buffers[descriptor_set_index];
        }

        // The workgroup size the shader was specialized to.  Shaders that size a shared array
        // from it need this to work out how much shared memory they actually ask the device for.
        const Size &getLocalWorkgroupSize() const {
            return m_local_workgroup_size;
        }

    private:
        friend class CommandBuffer;
        static const Size c_default_workgroup_size;
        static const Size c_default_linear_workgroup_size;

        void initialize(const std::vector<MemoryObjectType> &buffer_types, int max_descriptor_sets,
                        const vk::PhysicalDeviceLimits &limits);

        void createShaderModule();
        void createDescriptorLayout(int number_of_descriptor_sets, const std::vector<MemoryObjectType> &buffer_types,
                                    const vk::PhysicalDeviceLimits &limits);
        void updateDescriptorSet(int set_index);
        void createPipeline();
        void bindPipelineAndDescriptorSets(const vk::CommandBuffer &commandBuffer, int descriptor_set_index);

        template<typename T>
        void bindPushConstants(const vk::CommandBuffer &commandBuffer, const std::vector<T> &push_constants) {
            assert(push_constants.size() * sizeof(T) == m_push_constants_size);
            if (!push_constants.empty()) {
                commandBuffer.pushConstants(m_pipeline_layout,
                                            vk::ShaderStageFlagBits::eCompute,
                                            0,
                                            sizeof(T) * push_constants.size(),
                                            push_constants.data());
            }
        }

        void dispatch(const vk::CommandBuffer &commandBuffer, const vk::PhysicalDeviceLimits &limits);

        std::string m_name;
        vk::Device &m_device;
        size_t m_descriptor_count;
        size_t m_push_constants_size;
        std::vector<std::vector<std::shared_ptr<VulkanMemoryObject>>> m_buffers;

        vk::DescriptorSetLayout m_descriptor_set_layout;
        vk::DescriptorPool m_descriptor_pool; // TODO: think about whether it is good to have one per shader
        std::vector<vk::DescriptorSet> m_descriptor_sets;
        vk::ShaderModule m_shader_module;
        vk::PipelineLayout m_pipeline_layout;
        vk::PipelineCache m_pipeline_cache;
        vk::Pipeline m_pipeline;

        std::vector<uint32_t> m_spirv;
        // Values for specialization ids from 4 up; ids 1-3 are always the workgroup size.
        std::vector<uint32_t> m_specialization_constants;
        Size m_workgroup_size;
        Size m_local_workgroup_size;
    };
}

#endif //MUSECPP_COMPUTESHADER_H