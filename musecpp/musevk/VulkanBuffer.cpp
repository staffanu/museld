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
            : VulkanMemoryObject(vulkan_manager, size.numberOfElements() * element_size),
              m_vulkan_manager(vulkan_manager),
              m_size(size) {

        vk::BufferCreateInfo buffer_info(vk::BufferCreateFlags(),
                                         m_memory_size,
                                         buffer_usage_flags | vk::BufferUsageFlagBits::eTransferSrc, // FIXME: add dynamically if needed!
                                         vk::SharingMode::eExclusive);
        m_device_buffer = m_vulkan_manager.getDevice().createBuffer(buffer_info);

        m_descriptor_buffer_info = vk::DescriptorBufferInfo(m_device_buffer, 0, m_memory_size);

        vk::MemoryRequirements memory_requirements = m_vulkan_manager.getDevice().getBufferMemoryRequirements(m_device_buffer);

        allocateMemory(memory_requirements, host_access);

        m_vulkan_manager.getDevice().bindBufferMemory(m_device_buffer, m_allocated_device_memory.device_memory, m_allocated_device_memory.offset);
    }

    VulkanBuffer::~VulkanBuffer() {
        m_vulkan_manager.getDevice().destroy(m_device_buffer);
    }
}
