//
// Created by staffanu on 3/16/24.
//

#include "VulkanMemoryObject.h"

vk::MemoryPropertyFlags musevk::VulkanMemoryObject::makeMemoryPropertyFlags(HostAccess host_access) {
    // It is assumed that all memory will be accessed by the device.
    switch (host_access) {
        case HostAccess::eHostNone:
            return vk::MemoryPropertyFlagBits::eDeviceLocal;
        case HostAccess::eHostRead:
            return vk::MemoryPropertyFlagBits::eDeviceLocal |
                   vk::MemoryPropertyFlagBits::eHostVisible; // |
            vk::MemoryPropertyFlagBits::eHostCached;
        case HostAccess::eHostWrite:
            return vk::MemoryPropertyFlagBits::eDeviceLocal |
                   vk::MemoryPropertyFlagBits::eHostVisible |
                   vk::MemoryPropertyFlagBits::eHostCoherent;
        case HostAccess::eHostReadWrite:
            return vk::MemoryPropertyFlagBits::eDeviceLocal |
                   vk::MemoryPropertyFlagBits::eHostVisible |
                   vk::MemoryPropertyFlagBits::eHostCoherent |
                   vk::MemoryPropertyFlagBits::eHostCached;
        default:
            throw std::runtime_error("Undefined host_access type");
    }
}
