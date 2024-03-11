//
// Created by staffanu on 5/31/23.
//

#include "VulkanBuffer.h"
#include "VulkanManager.h"

namespace musevk {
    VulkanBuffer::VulkanBuffer(VulkanManager &vulkan_manager,
                               Size size,
                               uint32_t element_size,
                               vk::BufferUsageFlags buffer_usage_flags,
                               HostAccess host_access)
            : VulkanMemoryObject(vulkan_manager.getMemoryAllocator()),
              m_vulkan_manager(vulkan_manager),
              m_size(size),
              m_memory_size(size.numberOfElements() * element_size),
              m_allocated_memory() {

        vk::BufferCreateInfo buffer_info(vk::BufferCreateFlags(),
                                         m_memory_size,
                                         buffer_usage_flags,
                                         vk::SharingMode::eExclusive);
        m_buffer = m_vulkan_manager.getDevice().createBuffer(buffer_info);

        m_descriptor_buffer_info = vk::DescriptorBufferInfo(m_buffer, 0, m_memory_size);

        allocateAndBindMemory(makeMemoryPropertyFlags(host_access));

        m_raw_data = host_access != eHostNone ? m_allocated_memory.host_memory : nullptr;
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
