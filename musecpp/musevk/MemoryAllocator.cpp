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
            vk::MemoryRequirements memory_requirements, vk::MemoryPropertyFlags properties) {
        std::scoped_lock lock(m_mutex);

        int32_t memory_type_index = -1;
        vk::PhysicalDeviceMemoryProperties memory_properties = m_physical_device.getMemoryProperties();
        for (int32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
            if (memory_requirements.memoryTypeBits & (1 << i) &&
                (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
                memory_type_index = i;
                break;
            }
        }
        if (memory_type_index == -1) {
            printMemoryProperties(m_log, memory_properties);
            throw std::runtime_error(fmt::format("Memory type index for memory allocation not found; "
                                                 "memory_requirements(size={}, alignment={}, bits={}), "
                                                 "properties={}",
                                                 memory_requirements.size, memory_requirements.alignment,
                                                 memory_requirements.memoryTypeBits,
                                                 (uint32_t) properties));
        }

        // FIXME: why do we need to key on eHostVisible?  This should be part of the memory_type_index anyway
        bool is_host_visible = properties & vk::MemoryPropertyFlagBits::eHostVisible ? true : false;
        auto key = std::make_pair(memory_type_index, is_host_visible);

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

    void MemoryAllocator::printMemoryProperties(Logger &log, const VkPhysicalDeviceMemoryProperties &memProperties) {
        log.info(eVideo, "Available memory types");
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            const VkMemoryType &memType = memProperties.memoryTypes[i];
            log.info(eVideo, fmt::format("Memory Type {}: heap index {}, property flags({}): {}",
                                         i, memType.heapIndex, memType.propertyFlags, memoryPropertyFlagsToString(memType.propertyFlags)));
        }

        log.info(eVideo, "Available memory heaps:");
        for (uint32_t i = 0; i < memProperties.memoryHeapCount; ++i) {
            const VkMemoryHeap &memHeap = memProperties.memoryHeaps[i];
            log.info(eVideo, fmt::format("Memory Heap {}: size {}, flags({}): {}",
                                         i, memHeap.size, memHeap.flags, memoryHeapFlagsToString(memHeap.flags)));
        }
    }

    std::string MemoryAllocator::memoryPropertyFlagsToString(VkMemoryPropertyFlags flags) {
        std::vector<std::string> result;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT");
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT");
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_HOST_COHERENT_BIT");
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_HOST_CACHED_BIT");
        if (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT");
        if (flags & VK_MEMORY_PROPERTY_PROTECTED_BIT)
            result.emplace_back("VK_MEMORY_PROPERTY_PROTECTED_BIT");
        if (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)
            result.emplace_back("VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD");
        if (flags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)
            result.emplace_back("VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD");
        if (flags & VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV)
            result.emplace_back("VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV");

        std::ostringstream ss;
        std::string delim;
        for (auto &s : result) {
            ss << delim << s;
            delim = " | ";
        }
        return ss.str();
    }

    std::string MemoryAllocator::memoryHeapFlagsToString(VkMemoryHeapFlags flags) {
        std::string result;
        if (flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            result += "VK_MEMORY_HEAP_DEVICE_LOCAL_BIT";
        return result;
    }
}