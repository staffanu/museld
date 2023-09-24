//
// Created by staffanu on 6/11/23.
//

#ifndef MUSECPP_VULKANMEMORYOBJECT_H
#define MUSECPP_VULKANMEMORYOBJECT_H

#include <vulkan/vulkan.hpp>
#include <cstdint>
#include "MemoryAllocator.h"

namespace musevk {
    enum MemoryObjectType {
        eBuffer = 0,
        eImage = 1
    };

    class VulkanMemoryObject {
    public:
        VulkanMemoryObject(MemoryAllocator &memory_allocator)
        : m_memory_allocator(memory_allocator){
        }

        virtual MemoryObjectType getType() const = 0;
        virtual vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const = 0;

    protected:
        MemoryAllocator &m_memory_allocator;
    };
}

#endif //MUSECPP_VULKANMEMORYOBJECT_H
