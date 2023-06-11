//
// Created by staffanu on 6/11/23.
//

#ifndef MUSECPP_VULKANMEMORYOBJECT_H
#define MUSECPP_VULKANMEMORYOBJECT_H

#include <vulkan/vulkan.hpp>
#include <cstdint>

namespace musevk {
    enum MemoryObjectType {
        eBuffer = 0,
        eImage = 1
    };

    class VulkanMemoryObject {
    public:
        VulkanMemoryObject(vk::PhysicalDevice &physical_device)
        : m_physical_device(physical_device){
        }

        virtual MemoryObjectType getType() const = 0;
        virtual vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const = 0;

    protected:
        uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);

    private:
        vk::PhysicalDevice &m_physical_device;
    };
}

#endif //MUSECPP_VULKANMEMORYOBJECT_H
