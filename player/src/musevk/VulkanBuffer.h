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

    protected:
        void enqueueDeviceToHostVisibleCopy(CommandBuffer &command_buffer) final;
        void enqueueHostVisibleToDeviceCopy(CommandBuffer &command_buffer) final;
        void enqueueHostSyncBarrier(CommandBuffer &command_buffer,
                                    vk::AccessFlagBits src_access,
                                    vk::AccessFlagBits dst_access,
                                    vk::PipelineStageFlagBits src_stage,
                                    vk::PipelineStageFlagBits dst_stage) final;

    private:
        VulkanManager &m_vulkan_manager;
        Size m_size;

        vk::Buffer m_device_buffer;
        vk::DescriptorBufferInfo m_descriptor_buffer_info;
    };
}

#endif //MUSECPP_VULKANBUFFER_H
