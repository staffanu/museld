//
// Created by staffanu on 5/13/23.
//

#ifndef MUSECPP_MUSEBUFFER_H
#define MUSECPP_MUSEBUFFER_H

#include "musevk/CommandBuffer.h"

class MuseBuffer {
public:
    MuseBuffer(unsigned int height, unsigned int width, std::shared_ptr<musevk::VulkanBuffer> vulkanBuffer)
            : m_height(height), m_width(width), m_vulkan_buffer(vulkanBuffer) {
        assert(vulkanBuffer->size() == height * width);
    }

    unsigned int width() {
        return m_width;
    }

    unsigned int height() {
        return m_height;
    }

    std::shared_ptr<musevk::VulkanBuffer> getVulkanBuffer() {
        return m_vulkan_buffer;
    }

private:
    unsigned int m_height;
    unsigned int m_width;
    std::shared_ptr<musevk::VulkanBuffer> m_vulkan_buffer;
};

#endif //MUSECPP_MUSEBUFFER_H
