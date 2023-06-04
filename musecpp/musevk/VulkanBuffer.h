//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_VULKANBUFFER_H
#define MUSECPP_VULKANBUFFER_H

#include <vulkan/vulkan.hpp>
#include <memory>
#include <iostream>
#include <optional>

namespace musevk {
    class VulkanBuffer {
    public:
        VulkanBuffer(vk::PhysicalDevice &physical_device,
                     vk::Device &device,
                     uint32_t number_of_elements,
                     uint32_t element_size,
                     bool is_host,
                     bool allow_transfers);

        VulkanBuffer(const VulkanBuffer &other) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &other) = delete;
        VulkanBuffer(VulkanBuffer &&other) = delete;
        VulkanBuffer &operator=(VulkanBuffer &&other) = delete;

        ~VulkanBuffer();

        vk::Buffer &buffer() {
            return m_buffer;
        }

        void enqueueMemoryBarrier(
                const vk::CommandBuffer &commandBuffer,
                vk::AccessFlagBits srcAccessMask,
                vk::AccessFlagBits dstAccessMask,
                vk::PipelineStageFlagBits srcStageMask,
                vk::PipelineStageFlagBits dstStageMask) {

            vk::BufferMemoryBarrier bufferMemoryBarrier;
            bufferMemoryBarrier.buffer = m_buffer;
            bufferMemoryBarrier.size = m_memory_size;
            bufferMemoryBarrier.srcAccessMask = srcAccessMask;
            bufferMemoryBarrier.dstAccessMask = dstAccessMask;
            bufferMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            commandBuffer.pipelineBarrier(srcStageMask,
                                          dstStageMask,
                                          vk::DependencyFlags(),
                                          nullptr,
                                          bufferMemoryBarrier,
                                          nullptr);
        }

        [[nodiscard]] vk::DescriptorBufferInfo createDescriptorBufferInfo() {
            return {m_buffer, 0, m_memory_size};
        }

        [[nodiscard]] uint32_t size() const {
            return this->m_size;
        }

        [[nodiscard]] uint32_t getMemorySize() const {
            return m_memory_size;
        }

        template<typename T>
        T *data() {
            return (T*)m_raw_data;
        }

    private:
        void allocateAndBindMemory(vk::MemoryPropertyFlags memoryPropertyFlags);

        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        uint32_t m_size;
        uint32_t m_memory_size;
        bool m_is_host;
        void *m_raw_data;

        vk::Buffer m_buffer;
        vk::DeviceMemory m_device_memory;
    };
}

#endif //MUSECPP_VULKANBUFFER_H
