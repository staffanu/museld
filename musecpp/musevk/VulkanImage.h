//
// Created by staffanu on 6/9/23.
//

#ifndef MUSECPP_VULKANIMAGE_H
#define MUSECPP_VULKANIMAGE_H

#include <vulkan/vulkan.hpp>
#include "VulkanMemoryObject.h"
#include "CommandBuffer.h"

namespace musevk {
    class VulkanManager;
    class CommandBuffer;

    class VulkanImage final : public VulkanMemoryObject {
    public:
        VulkanImage(VulkanManager &vulkan_manager,
                    uint32_t width,
                    uint32_t height,
                    vk::ImageUsageFlags image_usage_flags,
                    HostAccess host_access);

        VulkanImage(const VulkanImage &other) = delete;
        VulkanImage &operator=(const VulkanImage &other) = delete;
        VulkanImage(VulkanImage &&other) = delete;
        VulkanImage &operator=(VulkanImage &&other) = delete;

        ~VulkanImage();

        vk::Image &image() {
            return m_image;
        }

        [[nodiscard]] MemoryObjectType getType() const final {
            return eImage;
        }

        void synchronizeForHostRead(CommandBuffer &command_buffer) final {
            assert(m_host_access == eHostRead || m_host_access == eHostReadWrite);
            if (m_host_visible_buffer.has_value()) {
                vk::BufferImageCopy region(0, m_width, m_height,
                                           vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                           vk::Offset3D(0, 0, 0), vk::Extent3D(m_width, m_height, 1));
                command_buffer.enqueueCopyImageToBuffer(m_image, m_layout, m_host_visible_buffer.value(), region);
            }
        };

        void synchronizeHostWrites(CommandBuffer &command_buffer) final {
            assert(m_host_access == eHostWrite || m_host_access == eHostWriteRarely || m_host_access == eHostReadWrite);
            if (m_host_visible_buffer.has_value()) {
                // Notice: this has never been tested so could be wrong!
                vk::BufferImageCopy region(0, m_width, m_height,
                                           vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                           vk::Offset3D(0, 0, 0), vk::Extent3D(m_width, m_height, 1));
                command_buffer.enqueueCopyBufferToImage(m_host_visible_buffer.value(), m_image, m_layout, region);
            }
        };

        [[nodiscard]] uint32_t getWidth() const {
            return m_width;
        }

        [[nodiscard]] uint32_t getHeight() const {
            return m_height;
        }

        [[nodiscard]] vk::ImageView getView() const {
            return m_view;
        }

        void enqueueTransitionLayout(CommandBuffer &command_buffer, vk::ImageLayout new_layout,
                                     vk::PipelineStageFlagBits src_stage, vk::PipelineStageFlagBits dst_stage,
                                     vk::AccessFlags src_access_mask, vk::AccessFlags dst_access_mask);

        vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const final {
            return {
                    descriptor_set,
                    binding_index,
                    0, // Destination array element
                    vk::DescriptorType::eStorageImage,
                    m_descriptor_image_info,
                    nullptr // Descriptor buffer info
            };
        }

    private:
        VulkanManager &m_vulkan_manager;
        uint32_t m_width;
        uint32_t m_height;

        vk::Image m_image;
        vk::ImageView m_view;
        vk::DescriptorImageInfo m_descriptor_image_info;
        vk::ImageLayout m_layout;
    };
}

#endif //MUSECPP_VULKANIMAGE_H
