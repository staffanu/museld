//
// Created by staffanu on 3/16/24.
//

#include <format>
#include <sstream>
#include "VulkanMemoryObject.h"
#include "VulkanManager.h"
#include "logging/Logger.h"

musevk::VulkanMemoryObject::~VulkanMemoryObject() {
    if (m_host_visible_buffer)
        m_vulkan_manager.getDevice().destroy(m_host_visible_buffer.value());

    m_vulkan_manager.getMemoryAllocator().free(m_allocated_device_memory);
    if (m_allocated_host_visible_memory)
        m_vulkan_manager.getMemoryAllocator().free(m_allocated_host_visible_memory.value());
};

std::vector<std::pair<vk::MemoryPropertyFlags, std::optional<vk::MemoryPropertyFlags>>>
musevk::VulkanMemoryObject::makeMemoryPropertyFlags(HostAccess host_access) {
    // For unified (non-separate-buffer) paths without HOST_COHERENT, callers must use flush/invalidate explicitly.
    switch (host_access) {
        case HostAccess::eHostNone:
            return {
                    std::make_pair(vk::MemoryPropertyFlagBits::eDeviceLocal, std::nullopt)
            };
        case HostAccess::eHostRead:
            return {
                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal |
                            vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCached,
                            std::nullopt),

                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            std::make_optional(
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCached |
                                    vk::MemoryPropertyFlagBits::eHostCoherent))
            };
        case HostAccess::eHostWrite:
            return {
                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal |
                            vk::MemoryPropertyFlagBits::eHostVisible,
                            std::nullopt),

                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            std::make_optional(
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent))
            };
        case HostAccess::eHostWriteRarely:
            return {
                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            std::make_optional(
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent))
            };
        case HostAccess::eHostReadWrite:
            return {
                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal |
                            vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCoherent |
                            vk::MemoryPropertyFlagBits::eHostCached,
                            std::nullopt),

                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            std::make_optional(
                                    vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCached |
                                    vk::MemoryPropertyFlagBits::eHostCoherent))
            };
        default:
            throw std::runtime_error("Undefined host_access type");
    }
}

std::optional<std::pair<int32_t, vk::MemoryPropertyFlags>> musevk::VulkanMemoryObject::getMemoryTypeIndex(vk::MemoryRequirements memory_requirements, vk::MemoryPropertyFlags properties) {
    std::optional<std::pair<int32_t, vk::MemoryPropertyFlags>> memory_type_index_and_properties = std::nullopt;
    vk::PhysicalDeviceMemoryProperties memory_properties = m_vulkan_manager.getPhysicalDevice().getMemoryProperties();
    for (int32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if (memory_requirements.memoryTypeBits & (1 << i) &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_index_and_properties = std::make_optional(std::make_pair(i, memory_properties.memoryTypes[i].propertyFlags));
            break;
        }
    }
    return memory_type_index_and_properties;
}

void musevk::VulkanMemoryObject::allocateMemory(vk::MemoryRequirements memory_requirements, HostAccess host_access) {
    auto &m_memory_allocator = m_vulkan_manager.getMemoryAllocator();

    auto memory_property_priorities = makeMemoryPropertyFlags(host_access);

    // Create a host visible buffer so that we can query its memory requirements; if not used we'll destroy it again
    vk::BufferCreateInfo buffer_info(vk::BufferCreateFlags(),
                                     m_memory_size,
                                     vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
                                     vk::SharingMode::eExclusive);
    vk::Buffer host_visible_buffer = m_vulkan_manager.getDevice().createBuffer(buffer_info);
    vk::MemoryRequirements host_visible_buffer_memory_requirements = m_vulkan_manager.getDevice().getBufferMemoryRequirements(host_visible_buffer);

    for (std::pair<vk::MemoryPropertyFlags, std::optional<vk::MemoryPropertyFlags>> device_and_host_memory_flags : memory_property_priorities) {
        std::optional<std::pair<int32_t, vk::MemoryPropertyFlags>> device_memory_type_index_and_properties = getMemoryTypeIndex(memory_requirements, device_and_host_memory_flags.first);
        if (!device_memory_type_index_and_properties)
            continue;

        std::optional<std::pair<int32_t, vk::MemoryPropertyFlags>> host_visible_memory_type_index_and_properties = std::nullopt;
        if (device_and_host_memory_flags.second) {
            host_visible_memory_type_index_and_properties = getMemoryTypeIndex(host_visible_buffer_memory_requirements, device_and_host_memory_flags.second.value());
            if (!host_visible_memory_type_index_and_properties)
                continue;
        }

        // This might be host visible, and if it is, we do not have a host_visible_memory_type_index.
        m_device_memory_properties = device_memory_type_index_and_properties->second;
        m_allocated_device_memory = m_memory_allocator.allocate(
                memory_requirements, device_memory_type_index_and_properties.value().first,
                (device_and_host_memory_flags.first & vk::MemoryPropertyFlagBits::eHostVisible) && !host_visible_memory_type_index_and_properties,
                (int)host_access);

        Logger &log = m_vulkan_manager.getLogger();
        vk::PhysicalDeviceMemoryProperties mem_props = m_vulkan_manager.getPhysicalDevice().getMemoryProperties();
        int32_t dev_type_idx = device_memory_type_index_and_properties->first;
        uint32_t dev_heap_idx = mem_props.memoryTypes[dev_type_idx].heapIndex;
        if (host_visible_memory_type_index_and_properties) {
            int32_t host_type_idx = host_visible_memory_type_index_and_properties->first;
            uint32_t host_heap_idx = mem_props.memoryTypes[host_type_idx].heapIndex;
            log.debug(eVideo, std::format("VulkanMemoryObject: {} bytes, hostAccess={} -> separate host-visible buffer (raw copies needed); device=type{}(heap{},{}) host=type{}(heap{},{})",
                m_memory_size, (int)host_access,
                dev_type_idx, dev_heap_idx, vk::to_string(device_memory_type_index_and_properties->second),
                host_type_idx, host_heap_idx, vk::to_string(host_visible_memory_type_index_and_properties->second)));
            m_host_memory_properties = host_visible_memory_type_index_and_properties.value().second;
            m_allocated_host_visible_memory = m_memory_allocator.allocate(host_visible_buffer_memory_requirements,
                                                                          host_visible_memory_type_index_and_properties.value().first, true,
                                                                          (int)host_access);
            m_vulkan_manager.getDevice().bindBufferMemory(host_visible_buffer, m_allocated_host_visible_memory.value().device_memory, m_allocated_host_visible_memory.value().offset);
            m_host_visible_buffer = host_visible_buffer;
        } else {
            if (host_access == eHostNone) {
                log.debug(eVideo, std::format("VulkanMemoryObject: {} bytes, hostAccess=none -> device-only; type{}(heap{},{})",
                    m_memory_size, dev_type_idx, dev_heap_idx,
                    vk::to_string(device_memory_type_index_and_properties->second)));
            } else {
                log.debug(eVideo, std::format("VulkanMemoryObject: {} bytes, hostAccess={} -> host-visible device-local (no raw copies); type{}(heap{},{})",
                    m_memory_size, (int)host_access, dev_type_idx, dev_heap_idx,
                    vk::to_string(device_memory_type_index_and_properties->second)));
            }
            m_vulkan_manager.getDevice().destroy(host_visible_buffer);
        }

        m_raw_data = host_access != eHostNone ?
                     m_allocated_host_visible_memory != std::nullopt ? m_allocated_host_visible_memory.value().host_memory
                                                                     : m_allocated_device_memory.host_memory
                                              : nullptr;
        return;
    }

    // If we reach the end of the loop no suitable memory was found
    Logger &log = m_vulkan_manager.getLogger();
    log.error(eVideo, std::format("No available memory for host access type {}", (int)host_access));
    vk::PhysicalDeviceMemoryProperties memory_properties = m_vulkan_manager.getPhysicalDevice().getMemoryProperties();

    log.error(eVideo, "Available memory types:");
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const vk::MemoryType &memType = memory_properties.memoryTypes[i];
        log.error(eVideo, std::format("Memory Type {}: heap index {}, property flags: {}",
                                      i, memType.heapIndex, vk::to_string(memType.propertyFlags)));
    }

    log.error(eVideo, "Available memory heaps:");
    for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
        const vk::MemoryHeap &memHeap = memory_properties.memoryHeaps[i];
        log.error(eVideo, std::format("Memory Heap {}: size {}, flags: {}",
                                      i, memHeap.size, vk::to_string(memHeap.flags)));
    }

    throw std::runtime_error(std::format("No available memory for host access type {}", (int)host_access));
}
