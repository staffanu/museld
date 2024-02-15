//
// Created by staffanu on 9/24/23.
//

#ifndef MUSECPP_MEMORYALLOCATOR_H
#define MUSECPP_MEMORYALLOCATOR_H

#include <map>
#include <optional>
#include <vulkan/vulkan.hpp>
#include <memory>

namespace musevk {
    struct AllocatedMemory {
        vk::DeviceMemory device_memory;
        vk::DeviceSize offset;
        vk::DeviceSize size;
        std::pair<int32_t, bool> allocation_key;
        void *host_memory; // non-null if host visible
    };

    /*
     * This is a very simple memory allocator for Vulkan memory that allocates larger chunks of memory
     * from Vulkan and then sub-allocates memory for buffers and images.  The logic is very simple: it
     * keeps one AllocationPool for each type of memory (determined by the memory type index and whether
     * the memory is host visible or not).  Host visible memory is always mapped.
     *
     * In each pool, there is a list of blocks -- one for each actual Vulkan allocation.  When there is
     * no room for an allocation request in any existing block, a new block is created.  Blocks are freed
     * when all sub-allocated regions of the block have been freed, and there is no re-use of memory in
     * the block before then.
     *
     * This should work well for applications that more or less allocate all memory on startup.  For more
     * dynamic use cases more care should be given to re-using freed memory regions.
     */
    class MemoryAllocator {
    public:
        MemoryAllocator(vk::PhysicalDevice &physical_device,
                        vk::Device &device)
                : m_physical_device(physical_device),
                  m_device(device),
                  m_memory_pools() {
        }

        MemoryAllocator(MemoryAllocator &other) = delete;

        AllocatedMemory allocate(vk::MemoryRequirements memory_requirements, vk::MemoryPropertyFlags properties);

        void free(AllocatedMemory memory);

        ~MemoryAllocator();

    private:
        struct MemoryBlock {
            vk::Device device;
            std::pair<int32_t, bool> allocation_key;
            vk::DeviceMemory device_memory;
            size_t block_size;
            size_t free_offset;
            std::vector<AllocatedMemory> allocations;
            void *host_memory; // non-null if host visible

            std::optional<AllocatedMemory> allocate(vk::MemoryRequirements memory_requirements);

            bool free(AllocatedMemory const &allocation);
        };

        struct AllocationPool {
            vk::Device device;
            std::pair<int32_t, bool> allocation_key;
            std::vector<MemoryBlock> blocks;

            AllocatedMemory allocate(vk::MemoryRequirements memory_requirements);
            void free(AllocatedMemory &allocation);
        };

        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        std::map<std::pair<int32_t, bool>, AllocationPool> m_memory_pools;
    };
}

#endif //MUSECPP_MEMORYALLOCATOR_H
