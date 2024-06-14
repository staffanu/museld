//
// Created by staffanu on 6/11/23.
//

#ifndef MUSECPP_VULKANMEMORYOBJECT_H
#define MUSECPP_VULKANMEMORYOBJECT_H

#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <vector>
#include <optional>
#include "MemoryAllocator.h"

namespace musevk {
    class VulkanManager;
    class CommandBuffer;

    enum MemoryObjectType {
        eBuffer = 0,
        eImage = 1
    };

    enum HostAccess {
        eHostNone = 0,
        eHostRead = 1,
        eHostWrite = 2, // try to allocate a host visible buffer if possible
        eHostWriteRarely = 3, // do not care about whether the buffer is host writable -- insist more on a separate device local buffer
        eHostReadWrite = 4
    };

    // VulkanMemoryObject represents an Image or a Vulkan Buffer.  It is assumed that all memory objects need to be
    // device local -- currently creating an object that is not device local at all is not supported.
    // That being said, the HostAccess flag given to the constructors of the subtypes indicates how the buffer will
    // be used.  If, for example, the buffer needs host read access, and there is no memory type available that supports
    // both eDeviceLocal and eHostVisible together with eHostCached, the buffer will actually represent two objects:
    // one on the CPU and one on the GPU.  Before accessing the buffer for reading on the CPU, we need to tell the object
    // about this, and it will issue a copy to the CPU visible buffer.  Since the caller doesn't know if this is the case,
    // before memory is read, calling synchronizeForHostRead needs to be called.  If there is only one underlying memory
    // object, this call is a nop, but if we use two buffers its content is copied.
    // For the same reason, it is required to call synchronizeHostWrites to ensure that host writes are visible from the GPU.
    class VulkanMemoryObject {
    public:
        explicit VulkanMemoryObject(VulkanManager &vulkan_manager, uint32_t memory_size, HostAccess host_access) :
          m_vulkan_manager(vulkan_manager),
          m_memory_size(memory_size),
          m_host_access(host_access),
          m_allocated_device_memory(),
          m_allocated_host_visible_memory(),
          m_raw_data(nullptr) {
        }

        [[nodiscard]] virtual MemoryObjectType getType() const = 0;

        [[nodiscard]] uint32_t getMemorySize() const {
            return m_memory_size;
        }

        template<typename T>
        T *data() {
            return (T*)m_raw_data;
        }

        virtual void synchronizeForHostRead(CommandBuffer &command_buffer) = 0;
        virtual void synchronizeHostWrites(CommandBuffer &command_buffer) = 0;

        virtual vk::WriteDescriptorSet
        makeWriteDescriptorSet(vk::DescriptorSet &descriptor_set, uint32_t binding_index) const = 0;

    protected:
        ~VulkanMemoryObject();

        // For a given host_access type, we return a list of possible memory property options.
        // The options are listed in preferred order, with the most preferred first.
        // Each option consists of a pair of memory property flags: the first item is for a buffer that
        // is device local, and the second one is for a host accessible buffer, in case that is required.
        // Notice that all buffers require at least one part that is device local.
        static std::vector<std::pair<vk::MemoryPropertyFlags, std::optional<vk::MemoryPropertyFlags>>>
                makeMemoryPropertyFlags(HostAccess host_access);

        // This will allocate device memory, and if required, also allocate a host visible buffer that
        // will be used for host read/writes in case there is no available memory type that is suitable
        // for using a device buffer only.  The allocated memory size might be larger than m_memory_size
        // indicates due to limits on allocation size.
        void allocateMemory(vk::MemoryRequirements memory_requirements, HostAccess host_access);

        VulkanManager &m_vulkan_manager;
        uint32_t m_memory_size;
        HostAccess m_host_access;
        vk::MemoryPropertyFlags m_device_memory_properties;
        AllocatedMemory m_allocated_device_memory;
        std::optional<vk::MemoryPropertyFlags> m_host_memory_properties;
        std::optional<AllocatedMemory> m_allocated_host_visible_memory;
        std::optional<vk::Buffer> m_host_visible_buffer;
        void *m_raw_data;

    private:
        std::optional<std::pair<int32_t, vk::MemoryPropertyFlags>> getMemoryTypeIndex(vk::MemoryRequirements memory_requirements, vk::MemoryPropertyFlags properties);
        static std::string memoryPropertyFlagsToString(VkMemoryPropertyFlags flags);
        static std::string memoryHeapFlagsToString(VkMemoryHeapFlags flags);
    };
}

#endif //MUSECPP_VULKANMEMORYOBJECT_H
