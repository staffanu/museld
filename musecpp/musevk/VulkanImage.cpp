//
// Created by staffanu on 6/9/23.
//

#include "VulkanImage.h"
#include "VulkanManager.h"
#include "CommandBuffer.h"

namespace musevk {
    VulkanImage::VulkanImage(VulkanManager &vulkan_manager,
                             uint32_t width,
                             uint32_t height,
                             vk::ImageUsageFlags image_usage_flags)
            : VulkanMemoryObject(vulkan_manager.getMemoryAllocator()),
              m_vulkan_manager(vulkan_manager),
              m_width(width),
              m_height(height),
              m_image(),
              m_allocated_memory(),
              m_view(),
              m_descriptor_image_info(),
              m_layout(vk::ImageLayout::eUndefined) {
        auto format = vk::Format::eB8G8R8A8Unorm;

        vk::ImageCreateInfo image_info(vk::ImageCreateFlags(),
                                       vk::ImageType::e2D,
                                       format,
                                       VkExtent3D {width, height, 1},
                                       1, // mipLevels
                                       1, // arrayLayers
                                       vk::SampleCountFlagBits::e1,
                                       vk::ImageTiling::eOptimal,
                                       image_usage_flags,
                                       vk::SharingMode::eExclusive,
                                       nullptr,
                                       m_layout);
        m_image = m_vulkan_manager.getDevice().createImage(image_info);

        auto memory_property_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
        allocateAndBindMemory(memory_property_flags);

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
        m_memory_allocator.free(m_allocated_memory);
        m_vulkan_manager.getDevice().destroy(m_view);
        m_vulkan_manager.getDevice().destroy(m_image);
    }

    void VulkanImage::enqueueTransitionLayout(CommandBuffer &command_buffer, vk::ImageLayout new_layout,
                                              vk::PipelineStageFlagBits src_stage, vk::PipelineStageFlagBits dst_stage,
                                              vk::AccessFlags src_access_mask, vk::AccessFlags dst_access_mask) {
        if (new_layout != m_layout) {
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

    void VulkanImage::allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags) {
        vk::MemoryRequirements memoryRequirements = m_vulkan_manager.getDevice().getImageMemoryRequirements(m_image);

        m_allocated_memory = m_memory_allocator.allocate(memoryRequirements, memory_property_flags);
        m_vulkan_manager.getDevice().bindImageMemory(m_image, m_allocated_memory.device_memory, m_allocated_memory.offset);
    }
}
