//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_VULKANBUFFER_H
#define MUSECPP_VULKANBUFFER_H

#include <vulkan/vulkan.hpp>
#include <memory>
#include "VulkanMemoryObject.h"
#include "Size.h"

namespace musevk {
    class VulkanManager;

    class VulkanBuffer final : public VulkanMemoryObject {
    public:
        VulkanBuffer(VulkanManager &vulkan_manager,
                     Size size,
                     uint32_t element_size,
                     bool is_host,
                     bool allow_transfers);  // only relevant for device local storage buffers
        VulkanBuffer(const VulkanBuffer &other) = delete;
        VulkanBuffer &operator=(const VulkanBuffer &other) = delete;
        VulkanBuffer(VulkanBuffer &&other) = delete;
        VulkanBuffer &operator=(VulkanBuffer &&other) = delete;

        ~VulkanBuffer() override;

        vk::Buffer &buffer() {
            return m_buffer;
        }

        [[nodiscard]] MemoryObjectType getType() const final {
            return eBuffer;
        }

        vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const final {
            return {
                    descriptor_set,
                    binding_index,
                    0, // Destination array element
                    vk::DescriptorType::eStorageBuffer,
                    nullptr, // Descriptor image info
                    m_descriptor_buffer_info
            };
        }

        [[nodiscard]] Size size() const {
            return m_size;
        }

        [[nodiscard]] uint32_t getMemorySize() const {
            return m_memory_size;
        }

        template<typename T>
        T *data() {
            return (T*)m_raw_data;
        }

    private:
        void allocateAndBindMemory(vk::MemoryPropertyFlags memory_property_flags);

        VulkanManager &m_vulkan_manager;
        Size m_size;
        uint32_t m_memory_size;
        bool m_is_host;
        void *m_raw_data;

        vk::Buffer m_buffer;
        AllocatedMemory m_allocated_memory;
        vk::DescriptorBufferInfo m_descriptor_buffer_info;
    };
}

#endif //MUSECPP_VULKANBUFFER_H
