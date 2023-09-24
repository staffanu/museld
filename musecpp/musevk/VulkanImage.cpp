//
// Created by staffanu on 6/9/23.
//

#include "VulkanImage.h"

namespace musevk {
    VulkanImage::VulkanImage(MemoryAllocator &memory_allocator,
                               vk::Device &device,
                               uint32_t width,
                               uint32_t height)
            : VulkanMemoryObject(memory_allocator),
              m_device(device),
              m_width(width),
              m_height(height) {
        auto image_usage_flags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc;

        vk::ImageCreateInfo image_info(vk::ImageCreateFlags(),
                                       vk::ImageType::e2D,
                                       vk::Format::eB8G8R8A8Unorm,
                                       VkExtent3D {width, height, 1},
                                       1, // mipLevels
                                       1, // arrayLayers
                                       vk::SampleCountFlagBits::e1,
                                       vk::ImageTiling::eLinear,
                                       image_usage_flags,
                                       vk::SharingMode::eExclusive,
                                       nullptr,
                                       vk::ImageLayout::eUndefined);
        m_image = m_device.createImage(image_info);

        auto memory_property_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
        allocateAndBindMemory(memory_property_flags);

        vk::ImageViewCreateInfo view_info(vk::ImageViewCreateFlags(),
                                          m_image,
                                          vk::ImageViewType::e2D,
                                          vk::Format::eB8G8R8A8Unorm,
                                          vk::ComponentMapping(),
                                          vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
        m_view = m_device.createImageView(view_info);

        m_descriptor_image_info = vk::DescriptorImageInfo(
                nullptr,
                m_view,
                vk::ImageLayout::eGeneral);
    }

    VulkanImage::~VulkanImage() {
        m_memory_allocator.free(m_allocated_memory);
        m_device.destroy(m_view);
        m_device.destroy(m_image);
    }

    void VulkanImage::allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags) {
        vk::MemoryRequirements memoryRequirements = m_device.getImageMemoryRequirements(m_image);

        m_allocated_memory = m_memory_allocator.allocate(memoryRequirements, memory_property_flags);
        m_device.bindImageMemory(m_image, m_allocated_memory.device_memory, m_allocated_memory.offset);
    }
}
