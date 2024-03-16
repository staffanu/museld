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

    enum HostAccess {
        eHostNone = 0,
        eHostRead = 1,
        eHostWrite = 2,
        eHostReadWrite = 3
    };

    class VulkanMemoryObject {
    public:
        explicit VulkanMemoryObject(MemoryAllocator &memory_allocator)
        : m_memory_allocator(memory_allocator),
          m_raw_data(nullptr) {
        }
        virtual ~VulkanMemoryObject() = default;

        [[nodiscard]] virtual MemoryObjectType getType() const = 0;

        template<typename T>
        T *data() {
            return (T*)m_raw_data;
        }

        virtual vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const = 0;

    protected:
        static vk::MemoryPropertyFlags makeMemoryPropertyFlags(HostAccess host_access);

        MemoryAllocator &m_memory_allocator;
        void *m_raw_data;
    };
}

#endif //MUSECPP_VULKANMEMORYOBJECT_H
