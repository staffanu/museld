//
// Created by staffanu on 5/31/23.
//

#include "VulkanBuffer.h"

namespace musevk {
    VulkanBuffer::VulkanBuffer(MemoryAllocator &memory_allocator,
                               vk::Device &device,
                               uint32_t number_of_elements,
                               uint32_t element_size,
                               bool is_host,
                               bool allow_transfers) // only relevant for device local storage buffers
            : VulkanMemoryObject(memory_allocator),
              m_device(device),
              m_size(number_of_elements),
              m_memory_size(number_of_elements * element_size),
              m_is_host(is_host),
              m_allocated_memory(),
              m_raw_data(nullptr) {

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

        m_raw_data = is_host ? m_allocated_memory.host_memory : nullptr;
    }

    VulkanBuffer::~VulkanBuffer() {
        m_device.destroy(m_buffer);
        m_memory_allocator.free(m_allocated_memory);
    }

    void VulkanBuffer::allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags) {
        vk::MemoryRequirements memory_requirements = m_device.getBufferMemoryRequirements(m_buffer);

        m_allocated_memory = m_memory_allocator.allocate(memory_requirements, memory_property_flags);
        m_device.bindBufferMemory(m_buffer, m_allocated_memory.device_memory, m_allocated_memory.offset);
    }
}
