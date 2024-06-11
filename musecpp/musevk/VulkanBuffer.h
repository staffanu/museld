//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_VULKANBUFFER_H
#define MUSECPP_VULKANBUFFER_H

#include <vulkan/vulkan.hpp>
#include <memory>
#include "VulkanMemoryObject.h"
#include "Size.h"
#include "CommandBuffer.h"

namespace musevk {
    class VulkanManager;

    class VulkanBuffer final : public VulkanMemoryObject {
    public:
        VulkanBuffer(VulkanManager &vulkan_manager,
                     Size size,
                     uint32_t element_size,
                     vk::BufferUsageFlags buffer_usage_flags,
                     HostAccess host_access);
        VulkanBuffer(const VulkanBuffer &other) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &other) = delete;
        VulkanBuffer(VulkanBuffer &&other) = delete;
        VulkanBuffer &operator=(VulkanBuffer &&other) = delete;

        ~VulkanBuffer();

        vk::Buffer &buffer() {
            return m_device_buffer;
        }

        [[nodiscard]] MemoryObjectType getType() const final {
            return eBuffer;
        }

        void synchronizeForHostRead(CommandBuffer &command_buffer) final {
            assert(m_host_access == eHostRead || m_host_access == eHostReadWrite);
            if (m_host_visible_buffer.has_value()) {
                command_buffer.enqueueBufferBarrier(*this,
                                                    vk::AccessFlagBits::eShaderWrite,
                                                    vk::AccessFlagBits::eTransferRead,
                                                    vk::PipelineStageFlagBits::eComputeShader,
                                                    vk::PipelineStageFlagBits::eTransfer);

                command_buffer.enqueueCopyRawBuffer(m_device_buffer, m_host_visible_buffer.value(), 0, 0,
                                                    m_memory_size);
            }
        };

        void synchronizeHostWrites(CommandBuffer &command_buffer) final {
            assert(m_host_access == eHostWrite || m_host_access == eHostWriteRarely || m_host_access == eHostReadWrite);
            if (m_host_visible_buffer.has_value())
                command_buffer.enqueueCopyRawBuffer(m_host_visible_buffer.value(), m_device_buffer, 0, 0, m_memory_size);
        };

        vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const final {
            return {
                    descriptor_set,
                    binding_index,
                    0, // Destination array element
                    vk::DescriptorType::eStorageBuffer,
                    nullptr, // Descriptor image info
                    m_descriptor_buffer_info
            };
        }

        [[nodiscard]] Size size() const {
            return m_size;
        }

    private:
        VulkanManager &m_vulkan_manager;
        Size m_size;

        vk::Buffer m_device_buffer;
        vk::DescriptorBufferInfo m_descriptor_buffer_info;
    };
}

#endif //MUSECPP_VULKANBUFFER_H
