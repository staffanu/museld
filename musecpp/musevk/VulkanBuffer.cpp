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
            : m_physical_device(physical_device),
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

        m_device_memory = vk::DeviceMemory();
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

    void VulkanBuffer::allocateAndBindMemory(vk::MemoryPropertyFlags memoryPropertyFlags) {
        vk::PhysicalDeviceMemoryProperties memoryProperties = m_physical_device.getMemoryProperties();
        vk::MemoryRequirements memoryRequirements = m_device.getBufferMemoryRequirements(m_buffer);

        uint32_t memoryTypeIndex = -1;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
            if (memoryRequirements.memoryTypeBits & (1 << i)) {
                if (((memoryProperties.memoryTypes[i]).propertyFlags & memoryPropertyFlags) == memoryPropertyFlags) {
                    memoryTypeIndex = i;
                    break;
                }
            }
        }
        if (memoryTypeIndex == -1) {
            throw std::runtime_error("Memory type index for buffer creation not found");
        }

        vk::MemoryAllocateInfo memoryAllocateInfo(memoryRequirements.size, memoryTypeIndex);
        m_device_memory = m_device.allocateMemory(memoryAllocateInfo);
        m_device.bindBufferMemory(m_buffer, m_device_memory, 0);
    }
}