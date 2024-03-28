//
// Created by staffanu on 3/16/24.
//

#include <fmt/format.h>
#include "VulkanMemoryObject.h"
#include "VulkanManager.h"

musevk::VulkanMemoryObject::~VulkanMemoryObject() {
    if (m_host_visible_buffer.has_value())
        m_vulkan_manager.getDevice().destroy(m_host_visible_buffer.value());

    m_vulkan_manager.getMemoryAllocator().free(m_allocated_device_memory);
    if (m_allocated_host_visible_memory.has_value())
        m_vulkan_manager.getMemoryAllocator().free(m_allocated_host_visible_memory.value());
};

std::vector<std::pair<vk::MemoryPropertyFlags, std::optional<vk::MemoryPropertyFlags>>>
musevk::VulkanMemoryObject::makeMemoryPropertyFlags(HostAccess host_access) {
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
                                    vk::MemoryPropertyFlagBits::eHostCached))
            };
        case HostAccess::eHostWrite:
            return {
                    std::make_pair(
                            vk::MemoryPropertyFlagBits::eDeviceLocal |
                            vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCoherent,
                            std::nullopt)
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
                            vk::MemoryPropertyFlagBits::eHostCached |
                            vk::MemoryPropertyFlagBits::eHostCoherent,
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

std::optional<int32_t> musevk::VulkanMemoryObject::getMemoryTypeIndex(vk::MemoryRequirements memory_requirements, vk::MemoryPropertyFlags properties) {
    std::optional<int32_t> memory_type_index = std::nullopt;
    vk::PhysicalDeviceMemoryProperties memory_properties = m_vulkan_manager.getPhysicalDevice().getMemoryProperties();
    for (int32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if (memory_requirements.memoryTypeBits & (1 << i) &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_index = std::make_optional(i);
            break;
        }
    }
    return memory_type_index;
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
        std::optional<int32_t> device_memory_type_index = getMemoryTypeIndex(memory_requirements, device_and_host_memory_flags.first);
        if (!device_memory_type_index.has_value())
            continue;

        std::optional<int32_t> host_visible_memory_type_index = std::nullopt;
        if (device_and_host_memory_flags.second.has_value()) {
            host_visible_memory_type_index = getMemoryTypeIndex(host_visible_buffer_memory_requirements, device_and_host_memory_flags.second.value());
            if (!host_visible_memory_type_index.has_value())
                continue;
        }

        // This might be host visible, and if it is, we do not have a host_visible_memory_type_index.
        m_allocated_device_memory = m_memory_allocator.allocate(
                memory_requirements, device_memory_type_index.value(),
                (device_and_host_memory_flags.first & vk::MemoryPropertyFlagBits::eHostVisible) && !host_visible_memory_type_index.has_value());

        if (host_visible_memory_type_index.has_value()) {
            m_allocated_host_visible_memory = m_memory_allocator.allocate(host_visible_buffer_memory_requirements,
                                                                          host_visible_memory_type_index.value(), true);
            m_vulkan_manager.getDevice().bindBufferMemory(host_visible_buffer, m_allocated_host_visible_memory.value().device_memory, m_allocated_host_visible_memory.value().offset);
            m_host_visible_buffer = host_visible_buffer;
        } else {
            m_vulkan_manager.getDevice().destroy(host_visible_buffer);
        }

        m_raw_data = host_access != eHostNone ?
                     m_allocated_host_visible_memory != std::nullopt ? m_allocated_host_visible_memory.value().host_memory
                                                                     : m_allocated_device_memory.host_memory
                                              : nullptr;
        return;
    }

    printMemoryProperties();
    throw std::runtime_error(fmt::format("No available memory for host access type {}", host_access));
}

void musevk::VulkanMemoryObject::printMemoryProperties() {
    Logger &log = m_vulkan_manager.getLogger();
    vk::PhysicalDeviceMemoryProperties memory_properties = m_vulkan_manager.getPhysicalDevice().getMemoryProperties();

    log.error(eVideo, "Available memory types");
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const VkMemoryType &memType = memory_properties.memoryTypes[i];
        log.info(eVideo, fmt::format("Memory Type {}: heap index {}, property flags({}): {}",
                                     i, memType.heapIndex, memType.propertyFlags, memoryPropertyFlagsToString(memType.propertyFlags)));
    }

    log.error(eVideo, "Available memory heaps:");
    for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i) {
        const VkMemoryHeap &memHeap = memory_properties.memoryHeaps[i];
        log.info(eVideo, fmt::format("Memory Heap {}: size {}, flags({}): {}",
                                     i, memHeap.size, memHeap.flags, memoryHeapFlagsToString(memHeap.flags)));
    }
}

std::string musevk::VulkanMemoryObject::memoryPropertyFlagsToString(VkMemoryPropertyFlags flags) {
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

std::string musevk::VulkanMemoryObject::memoryHeapFlagsToString(VkMemoryHeapFlags flags) {
    std::string result;
    if (flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        result += "VK_MEMORY_HEAP_DEVICE_LOCAL_BIT";
    return result;
}