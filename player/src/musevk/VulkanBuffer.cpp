// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "VulkanBuffer.h"
#include "VulkanManager.h"

namespace musevk {
    VulkanBuffer::VulkanBuffer(VulkanManager &vulkan_manager,
                               Size size,
                               uint32_t element_size,
                               vk::BufferUsageFlags buffer_usage_flags,
                               HostAccess host_access)
            : VulkanMemoryObject(vulkan_manager, size.numberOfElements() * element_size, host_access),
              m_vulkan_manager(vulkan_manager),
              m_size(size) {

        // We add the src and dst usage flags so that we can use a separate host visible buffer for host
        // read/writes if necessary.  We could create one buffer for each case when trying the alternatives
        // for the specified host_access, but hopefully there is no real performance penalty for enabling these
        // flags.
        vk::BufferCreateInfo buffer_info(vk::BufferCreateFlags(),
                                         m_memory_size,
                                         buffer_usage_flags | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                                         vk::SharingMode::eExclusive);
        m_device_buffer = m_vulkan_manager.getDevice().createBuffer(buffer_info);

        vk::MemoryRequirements memory_requirements = m_vulkan_manager.getDevice().getBufferMemoryRequirements(m_device_buffer);

        allocateMemory(memory_requirements, host_access);

        m_vulkan_manager.getDevice().bindBufferMemory(m_device_buffer, m_allocated_device_memory.device_memory, m_allocated_device_memory.offset);

        m_descriptor_buffer_info = vk::DescriptorBufferInfo(m_device_buffer, 0, m_memory_size);
    }

    VulkanBuffer::~VulkanBuffer() {
        m_vulkan_manager.getDevice().destroy(m_device_buffer);
    }


    void VulkanBuffer::enqueueDeviceToHostVisibleCopy(CommandBuffer &command_buffer) {
        command_buffer.enqueueBufferBarrier(*this,
                                            vk::AccessFlagBits::eShaderWrite,
                                            vk::AccessFlagBits::eTransferRead,
                                            vk::PipelineStageFlagBits::eComputeShader,
                                            vk::PipelineStageFlagBits::eTransfer);
        command_buffer.enqueueCopyRawBuffer(m_device_buffer, m_host_visible_buffer.value(), 0, 0,
                                            m_memory_size);
    }

    void VulkanBuffer::enqueueHostSyncBarrier(CommandBuffer &command_buffer,
                                              vk::AccessFlagBits src_access,
                                              vk::AccessFlagBits dst_access,
                                              vk::PipelineStageFlagBits src_stage,
                                              vk::PipelineStageFlagBits dst_stage) {
        command_buffer.enqueueBufferBarrier(*this, src_access, dst_access, src_stage, dst_stage);
    }

    void VulkanBuffer::enqueueHostVisibleToDeviceCopy(CommandBuffer &command_buffer) {
        command_buffer.enqueueCopyRawBuffer(m_host_visible_buffer.value(), m_device_buffer, 0, 0,
                                            m_memory_size);
        command_buffer.enqueueBufferBarrier(*this,
                                            vk::AccessFlagBits::eTransferWrite,
                                            vk::AccessFlagBits::eShaderRead,
                                            vk::PipelineStageFlagBits::eTransfer,
                                            vk::PipelineStageFlagBits::eComputeShader);
    }
}
