//
// Created by staffanu on 5/31/23.
//

#include "VulkanBuffer.h"
#include "VulkanManager.h"

namespace musevk {
    VulkanBuffer::VulkanBuffer(VulkanManager &vulkan_manager,
                               Size size,
                               uint32_t element_size,
                               bool is_host,
                               bool allow_transfers) // only relevant for device local storage buffers
            : VulkanMemoryObject(vulkan_manager.getMemoryAllocator()),
              m_vulkan_manager(vulkan_manager),
              m_size(size),
              m_memory_size(size.numberOfElements() * element_size),
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
        m_buffer = m_vulkan_manager.getDevice().createBuffer(buffer_info);

        m_descriptor_buffer_info = vk::DescriptorBufferInfo(m_buffer, 0, m_memory_size);

        allocateAndBindMemory(memory_property_flags);

        m_raw_data = is_host ? m_allocated_memory.host_memory : nullptr;
    }

    VulkanBuffer::~VulkanBuffer() {
        m_vulkan_manager.getDevice().destroy(m_buffer);
        m_memory_allocator.free(m_allocated_memory);
    }

    void VulkanBuffer::allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags) {
        vk::MemoryRequirements memory_requirements = m_vulkan_manager.getDevice().getBufferMemoryRequirements(m_buffer);

        m_allocated_memory = m_memory_allocator.allocate(memory_requirements, memory_property_flags);
        m_vulkan_manager.getDevice().bindBufferMemory(m_buffer, m_allocated_memory.device_memory, m_allocated_memory.offset);
    }
}
