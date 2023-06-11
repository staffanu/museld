//
// Created by staffanu on 6/11/23.
//

#include "VulkanMemoryObject.h"

namespace musevk {
    uint32_t VulkanMemoryObject::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
        vk::PhysicalDeviceMemoryProperties memoryProperties = m_physical_device.getMemoryProperties();
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
            if (typeFilter & (1 << i) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("Memory type index for buffer creation not found");
    }
}
