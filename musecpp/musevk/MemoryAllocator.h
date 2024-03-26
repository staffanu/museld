//
// Created by staffanu on 9/24/23.
//

#ifndef MUSECPP_MEMORYALLOCATOR_H
#define MUSECPP_MEMORYALLOCATOR_H

#include <map>
#include <optional>
#include <vulkan/vulkan.hpp>
#include <memory>
#include <mutex>
#include "../util/Logger.h"

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
     *
     * The allocate and free methods are synchronized on an internal lock so should be thread safe.
     */
    class MemoryAllocator {
    public:
        MemoryAllocator(vk::PhysicalDevice &physical_device,
                        vk::Device &device)
                : m_physical_device(physical_device),
                  m_device(device),
                  m_memory_pools(),
                  m_mutex() {
        }

        MemoryAllocator(MemoryAllocator &other) = delete;
        MemoryAllocator &operator=(MemoryAllocator &other) = delete;

        // The reason that we need to specify host_visible here is that we use different chunks of memory for host visible and
        // non-host visible memory even if they have the same memory type index.  It could very well be that some different
        // HostAccess enumeration members map tp the same memory index, but that some do not require host visible memory, and
        // then we do not need to map memory for the ones that do not need it.
        AllocatedMemory allocate(vk::MemoryRequirements memory_requirements, int32_t memory_type_index, bool host_visible);

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
        std::mutex m_mutex;
    };
}

#endif //MUSECPP_MEMORYALLOCATOR_H
