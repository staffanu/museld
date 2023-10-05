//
// Created by staffanu on 6/9/23.
//

#ifndef MUSECPP_VULKANIMAGE_H
#define MUSECPP_VULKANIMAGE_H

#include <vulkan/vulkan.hpp>
#include "VulkanMemoryObject.h"

namespace musevk {
    class CommandBuffer;

    class VulkanImage final : public VulkanMemoryObject {
    public:
        VulkanImage(MemoryAllocator &memory_allocator,
                    vk::Device &device,
                    uint32_t width,
                    uint32_t height);

        VulkanImage(const VulkanImage &other) = delete;
        VulkanImage &operator=(const VulkanImage &other) = delete;
        VulkanImage(VulkanImage &&other) = delete;
        VulkanImage &operator=(VulkanImage &&other) = delete;

        ~VulkanImage();

        vk::Image &image() {
            return m_image;
        }

        MemoryObjectType getType() const final {
            return eImage;
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
        void allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags);

        vk::Device &m_device;
        uint32_t m_width;
        uint32_t m_height;

        vk::Image m_image;
        AllocatedMemory m_allocated_memory;
        vk::ImageView m_view;
        vk::DescriptorImageInfo m_descriptor_image_info;
        vk::ImageLayout m_layout;
    };
}

#endif //MUSECPP_VULKANIMAGE_H
