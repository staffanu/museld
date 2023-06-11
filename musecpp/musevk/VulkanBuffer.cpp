//
// Created by staffanu on 5/31/23.
//

#include "VulkanBuffer.h"

namespace musevk {
    VulkanBuffer::VulkanBuffer(vk::PhysicalDevice &physical_device,
                               vk::Device &device,
                               uint32_t number_of_elements,
                               uint32_t element_size,
                               bool is_host,
                               bool allow_transfers) // only relevant for device local storage buffers
            : VulkanMemoryObject(physical_device),
              m_device(device),
              m_size(number_of_elements),
              m_memory_size(number_of_elements * element_size),
              m_is_host(is_host) {
        auto buffer_usage_flags = m_is_host || allow_transfers ?
                                  vk::BufferUsageFlagBits::eStorageBuffer |
                                  vk::BufferUsageFlagBits::eTransferSrc |
                                  vk::BufferUsageFlagBits::eTransferDst :
                                  vk::BufferUsageFlagBits::eStorageBuffer;

        auto memory_property_flags = m_is_host ?
                                     vk::MemoryPropertyFlagBits::eHostVisible |
                                     vk::MemoryPropertyFlagBits::eHostCoherent :
                                     vk::MemoryPropertyFlagBits::eDeviceLocal;

        vk::BufferCreateInfo buffer_info(vk::BufferCreateFlags(),
                                         m_memory_size,
                                         buffer_usage_flags,
                                         vk::SharingMode::eExclusive);
        m_buffer = m_device.createBuffer(buffer_info);

        m_descriptor_buffer_info = vk::DescriptorBufferInfo(m_buffer, 0, m_memory_size);

        allocateAndBindMemory(memory_property_flags);

        if (is_host)
            m_raw_data = m_device.mapMemory(m_device_memory, 0, m_memory_size, vk::MemoryMapFlags());
        else
            m_raw_data = nullptr;
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_is_host) {
            vk::MappedMemoryRange mappedRange(m_device_memory, 0, m_memory_size);
            m_device.flushMappedMemoryRanges(mappedRange);
            m_device.unmapMemory(m_device_memory);
        }
        m_device.destroy(m_buffer);
        m_device.freeMemory(m_device_memory);
    }

    void VulkanBuffer::allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags) {
        vk::MemoryRequirements memoryRequirements = m_device.getBufferMemoryRequirements(m_buffer);

        uint32_t memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, memory_property_flags);

        vk::MemoryAllocateInfo memoryAllocateInfo(memoryRequirements.size, memoryTypeIndex);
        m_device_memory = m_device.allocateMemory(memoryAllocateInfo);
        m_device.bindBufferMemory(m_buffer, m_device_memory, 0);
    }
}
