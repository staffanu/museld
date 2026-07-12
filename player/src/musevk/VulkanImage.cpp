// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#include "VulkanImage.h"
#include "VulkanManager.h"
#include "CommandBuffer.h"

namespace musevk {
    VulkanImage::VulkanImage(VulkanManager &vulkan_manager,
                             uint32_t width,
                             uint32_t height,
                             vk::ImageUsageFlags image_usage_flags,
                             HostAccess host_access)
            : VulkanMemoryObject(vulkan_manager, 4 * width * height, host_access),
              m_vulkan_manager(vulkan_manager),
              m_width(width),
              m_height(height),
              m_image(),
              m_view(),
              m_descriptor_image_info(),
              m_layout(vk::ImageLayout::eUndefined) {
        auto format = vk::Format::eR16G16B16A16Sfloat;

        vk::ImageCreateInfo image_info(vk::ImageCreateFlags(),
                                       vk::ImageType::e2D,
                                       format,
                                       VkExtent3D {width, height, 1},
                                       1, // mipLevels
                                       1, // arrayLayers
                                       vk::SampleCountFlagBits::e1,
                                       vk::ImageTiling::eOptimal, // TODO: automatically change to eLinear if host readable and not using a secondary buffer, or maybe add HostAccess enum value
                                       image_usage_flags,
                                       vk::SharingMode::eExclusive,
                                       nullptr,
                                       m_layout);
        m_image = m_vulkan_manager.getDevice().createImage(image_info);

        vk::MemoryRequirements memory_requirements = m_vulkan_manager.getDevice().getImageMemoryRequirements(m_image);

        allocateMemory(memory_requirements, host_access);

        m_vulkan_manager.getDevice().bindImageMemory(m_image, m_allocated_device_memory.device_memory, m_allocated_device_memory.offset);

        vk::ImageViewCreateInfo view_info(vk::ImageViewCreateFlags(),
                                          m_image,
                                          vk::ImageViewType::e2D,
                                          format,
                                          vk::ComponentMapping(),
                                          vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        m_view = m_vulkan_manager.getDevice().createImageView(view_info);

        m_descriptor_image_info = vk::DescriptorImageInfo(
                nullptr,
                m_view,
                m_layout);
    }

    VulkanImage::~VulkanImage() {
        m_vulkan_manager.getDevice().destroy(m_view);
        m_vulkan_manager.getDevice().destroy(m_image);
    }

    void VulkanImage::enqueueDeviceToHostVisibleCopy(CommandBuffer &command_buffer) {
        command_buffer.enqueueTransitionMemoryLayout(m_image,
                                                     m_layout, m_layout,
                                                     vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                                                     vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
        vk::BufferImageCopy region(0, m_width, m_height,
                                   vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                   vk::Offset3D(0, 0, 0), vk::Extent3D(m_width, m_height, 1));
        command_buffer.enqueueCopyImageToBuffer(m_image, m_layout, m_host_visible_buffer.value(), region);
    }

    void VulkanImage::enqueueHostSyncBarrier(CommandBuffer &command_buffer,
                                             vk::AccessFlagBits src_access,
                                             vk::AccessFlagBits dst_access,
                                             vk::PipelineStageFlagBits src_stage,
                                             vk::PipelineStageFlagBits dst_stage) {
        // For images we use a global memory barrier; the unified path is not exercised in practice
        // because images use eOptimal tiling and fall through to the separate-host-visible-buffer path.
        command_buffer.enqueueBarrier(src_access, dst_access, src_stage, dst_stage);
    }

    void VulkanImage::enqueueHostVisibleToDeviceCopy(CommandBuffer &command_buffer) {
        // Notice: this has never been tested so could be wrong!
        vk::BufferImageCopy region(0, m_width, m_height,
                                   vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                   vk::Offset3D(0, 0, 0), vk::Extent3D(m_width, m_height, 1));
        command_buffer.enqueueCopyBufferToImage(m_host_visible_buffer.value(), m_image, m_layout, region);
        command_buffer.enqueueTransitionMemoryLayout(m_image,
                                                     m_layout, m_layout,
                                                     vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                                                     vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead);
    }

    void VulkanImage::enqueueTransitionLayout(CommandBuffer &command_buffer, vk::ImageLayout new_layout,
                                              vk::PipelineStageFlagBits src_stage, vk::PipelineStageFlagBits dst_stage,
                                              vk::AccessFlags src_access_mask, vk::AccessFlags dst_access_mask) {
        command_buffer.enqueueTransitionMemoryLayout(m_image,
                                                     m_layout, new_layout,
                                                     src_stage, dst_stage,
                                                     src_access_mask, dst_access_mask);
        m_layout = new_layout;

        m_descriptor_image_info = vk::DescriptorImageInfo(
                nullptr,
                m_view,
                m_layout);
    }
}
