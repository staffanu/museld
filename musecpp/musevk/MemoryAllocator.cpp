//
// Created by staffanu on 9/24/23.
//

#include <iostream>
#include <vector>
#include <fmt/format.h>
#include "MemoryAllocator.h"
#include "../util/Logger.h"

#define ALLOCATION_BLOCK_SIZE (16 * 1024 * 1024)

namespace musevk {
    AllocatedMemory MemoryAllocator::allocate(
            vk::MemoryRequirements memory_requirements, int32_t memory_type_index, bool host_visible) {
        std::scoped_lock lock(m_mutex);

        auto key = std::make_pair(memory_type_index, host_visible);

        if (m_memory_pools.find(key) == m_memory_pools.end())
            m_memory_pools[key] = AllocationPool{m_device, key, {}};

        return m_memory_pools[key].allocate(memory_requirements);
    }

    void MemoryAllocator::free(AllocatedMemory memory) {
        std::scoped_lock lock(m_mutex);
        m_memory_pools[memory.allocation_key].free(memory);
    }

    MemoryAllocator::~MemoryAllocator() {
        for (auto i: m_memory_pools)
            assert(i.second.blocks.empty());
    }

    std::optional<AllocatedMemory> MemoryAllocator::MemoryBlock::allocate(vk::MemoryRequirements memory_requirements) {
        auto alignment = memory_requirements.alignment;
        auto rounded_offset = (free_offset + alignment - 1) / alignment * alignment;
        if (rounded_offset + memory_requirements.size <= block_size) {
            AllocatedMemory allocated { device_memory, rounded_offset, memory_requirements.size, allocation_key,
                                        allocation_key.second ? (char *)host_memory + rounded_offset : nullptr };
            free_offset = rounded_offset + memory_requirements.size;
            allocations.push_back(allocated);
            return std::optional(allocated);
        } else
            return std::nullopt;
    }

    bool MemoryAllocator::MemoryBlock::free(AllocatedMemory const &allocation) {
        auto it = std::find_if(allocations.cbegin(), allocations.cend(), [allocation](AllocatedMemory const &i){
            return i.device_memory == allocation.device_memory && i.offset == allocation.offset;});

        if (it != allocations.cend()) {
            allocations.erase(it);
            return true;
        } else
            return false;
    }

    AllocatedMemory MemoryAllocator::AllocationPool::allocate(vk::MemoryRequirements memory_requirements) {
        for (auto it = blocks.begin(); it != blocks.end(); it++) {
            auto allocationOpt = it->allocate(memory_requirements);
            if (allocationOpt.has_value())
                return allocationOpt.value();
        }
        vk::MemoryAllocateInfo memoryAllocateInfo(ALLOCATION_BLOCK_SIZE, allocation_key.first);
        auto device_memory = device.allocateMemory(memoryAllocateInfo);

        auto ptr = allocation_key.second ?
                   device.mapMemory(device_memory, 0, VK_WHOLE_SIZE, vk::MemoryMapFlags()) : nullptr;

        MemoryBlock block {device, allocation_key, device_memory, ALLOCATION_BLOCK_SIZE, 0, {}, ptr};
        auto allocationOpt = block.allocate(memory_requirements);
        blocks.push_back(block);

        assert(allocationOpt.has_value());
        return allocationOpt.value();
    }

    void MemoryAllocator::AllocationPool::free(AllocatedMemory &allocation) {
        auto it = std::find_if(blocks.begin(), blocks.end(), [allocation](MemoryBlock &i){return i.free(allocation);});
        if (it != blocks.end()) {
            if (it->allocations.empty()) {
                it->device.freeMemory(it->device_memory);
                blocks.erase(it);
            }
        } else
            throw std::runtime_error("free() for unknown allocation");
    }
}