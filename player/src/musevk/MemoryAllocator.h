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
#include "logging/Logger.h"

namespace musevk {
    struct AllocatedMemory {
        vk::DeviceMemory device_memory;
        vk::DeviceSize offset;
        vk::DeviceSize size;
        std::tuple<int32_t, bool, int> allocation_key;
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
                  m_non_coherent_atom_size(m_physical_device.getProperties().limits.nonCoherentAtomSize),
                  m_memory_pools(),
                  m_mutex() {

        }

        MemoryAllocator(MemoryAllocator &other) = delete;
        MemoryAllocator &operator=(MemoryAllocator &other) = delete;

        // host_visible: distinct from memory_type_index because some HostAccess kinds map to the same memory type
        // but do not require the memory to be mapped; we keep those in a separate pool so we don't waste a mapping.
        //
        // pool_tag: a caller-chosen tag (typically the HostAccess enum value) that further partitions pools sharing
        // the same memory_type_index and host_visible flag.  This was added due to problems on hardware without
        // any DEVICE_LOCAL+HOST_COHERENT memory type, where both eHostWrite and eHostRead VulkanBuffers use
        // the same memory_type_index.  Specifically, on AMD Vega on an Intel Mac 2017, it seems that performing a flush of
        // one sub-range and an invalidation of another sub-range between submit/waits does not work correctly.
        // Separating the HostAccess types in different memory blocks avoids this problem.
        AllocatedMemory allocate(vk::MemoryRequirements memory_requirements, int32_t memory_type_index, bool host_visible, int pool_tag);

        void free(AllocatedMemory memory);

        ~MemoryAllocator();

    private:
        struct MemoryBlock {
            vk::Device device;
            std::tuple<int32_t, bool, int> allocation_key;
            vk::DeviceMemory device_memory;
            size_t block_size;
            size_t free_offset;
            std::vector<AllocatedMemory> allocations;
            void *host_memory; // non-null if host visible
            vk::DeviceSize m_non_coherent_atom_size;

            std::optional<AllocatedMemory> allocate(vk::MemoryRequirements memory_requirements);

            bool free(AllocatedMemory const &allocation);
        };

        struct AllocationPool {
            vk::Device device;
            std::tuple<int32_t, bool, int> allocation_key;
            std::vector<MemoryBlock> blocks;
            vk::DeviceSize m_non_coherent_atom_size;

            AllocatedMemory allocate(vk::MemoryRequirements memory_requirements);
            void free(AllocatedMemory &allocation);
        };

        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        vk::DeviceSize m_non_coherent_atom_size;
        std::map<std::tuple<int32_t, bool, int>, AllocationPool> m_memory_pools;
        std::mutex m_mutex;
    };
}

#endif //MUSECPP_MEMORYALLOCATOR_H
