// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <stdexcept>
#include <format>
#include <algorithm>
#include <iostream>
#include <vector>
#include "MemoryAllocator.h"

#define ALLOCATION_BLOCK_SIZE (16 * 1024 * 1024)

namespace musevk {
    AllocatedMemory MemoryAllocator::allocate(
            vk::MemoryRequirements memory_requirements, int32_t memory_type_index, bool host_visible, int pool_tag) {
        std::scoped_lock lock(m_mutex);

        auto key = std::make_tuple(memory_type_index, host_visible, pool_tag);

        if (m_memory_pools.find(key) == m_memory_pools.end())
            m_memory_pools[key] = AllocationPool{m_device, key, {}, m_non_coherent_atom_size};

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

        auto rounded_size = (memory_requirements.size + m_non_coherent_atom_size - 1) / m_non_coherent_atom_size * m_non_coherent_atom_size;

        if (rounded_offset + rounded_size <= block_size) {
            AllocatedMemory allocated { device_memory, rounded_offset, rounded_size, allocation_key,
                                        std::get<1>(allocation_key) ? (char *)host_memory + rounded_offset : nullptr };
            free_offset = rounded_offset + rounded_size;
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
            if (allocationOpt)
                return allocationOpt.value();
        }
        // A new block: the standard size, or one of the request's own size when that is
        // larger (the MUSE frame buffers on the Apple TV's A10X, where MoltenVK's memory
        // requirements for them exceed 16 MB; they fit the standard block on macOS)
        const size_t needed = (memory_requirements.size + m_non_coherent_atom_size - 1)
                              / m_non_coherent_atom_size * m_non_coherent_atom_size
                              + memory_requirements.alignment;
        const size_t block_size = std::max<size_t>(ALLOCATION_BLOCK_SIZE, needed);
        vk::MemoryAllocateInfo memoryAllocateInfo(block_size, std::get<0>(allocation_key));
        auto device_memory = device.allocateMemory(memoryAllocateInfo);

        auto ptr = std::get<1>(allocation_key) ?
                   device.mapMemory(device_memory, 0, VK_WHOLE_SIZE, vk::MemoryMapFlags()) : nullptr;

        MemoryBlock block {device, allocation_key, device_memory, block_size, 0, {}, ptr, m_non_coherent_atom_size};
        auto allocationOpt = block.allocate(memory_requirements);
        blocks.push_back(block);

        if (!allocationOpt)
            throw std::runtime_error(std::format(
                    "MemoryAllocator: a request of {} bytes (alignment {}) does not fit a new block of {} bytes",
                    (uint64_t)memory_requirements.size, (uint64_t)memory_requirements.alignment, (uint64_t)block_size));
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