//
// Created by staffanu on 2/25/24.
//

#ifndef MUSECPP_VULKANUTIL_H
#define MUSECPP_VULKANUTIL_H

#include <vector>
#include "VulkanBuffer.h"
#include "VulkanManager.h"
#include "CommandPool.h"

namespace musevk {
    class VulkanUtil {
    public:
        static std::vector<uint32_t> loadSpirv(std::string const &executable_dir, std::string const &filename);

        template<typename T>
        static std::unique_ptr<VulkanBuffer> createDeviceBuffer(VulkanManager &vulkan_manager, CommandPool &command_pool, Size const &size, const std::vector<T> &data) {
            assert(size.numberOfElements() == data.size());
            auto host_buffer = VulkanBuffer(vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eTransferSrc, eHostWrite);
            for (int i = 0; i < data.size(); i++)
                host_buffer.data<T>()[i] = data[i];
            auto device_buffer = std::make_unique<VulkanBuffer>(
                    vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, eHostNone);
            auto sq = command_pool.createCommandBuffer();
            sq->begin();
            sq->enqueueCopyBuffer(host_buffer, *device_buffer);
            sq->submit({}, {}, {});
            sq->wait();
            return device_buffer;
        }

        template<typename T, size_t n>
        static std::unique_ptr<VulkanBuffer> createDeviceBuffer(VulkanManager &vulkan_manager, CommandPool &command_pool, Size const &size, const std::array<T, n> &data) {
            assert(size.numberOfElements() == n);
            auto host_buffer = VulkanBuffer(vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eTransferSrc, eHostWrite);
            for (int i = 0; i < data.size(); i++)
                host_buffer.data<T>()[i] = data[i];
            auto device_buffer = std::make_unique<VulkanBuffer>(
                    vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst, eHostNone);
            auto sq = command_pool.createCommandBuffer();
            sq->begin();
            sq->enqueueCopyBuffer(host_buffer, *device_buffer);
            sq->submit({}, {}, {});
            sq->wait();
            return device_buffer;
        }

        static std::unique_ptr<VulkanBuffer> createDeviceBufferFloatsAsHalfFloats(VulkanManager &vulkan_manager, CommandPool &command_pool,
                                                                                  Size const &size, const std::vector<float> &data) {
            assert(size.numberOfElements() == data.size());
            std::vector<ushort> half_floats(data.size());
            for (int i = 0; i < data.size(); i++)
                half_floats[i] = HalfFloatUtil::float_to_half(data[i]);
            return createDeviceBuffer<ushort>(vulkan_manager, command_pool, size, half_floats);
        }
    };
}


#endif //MUSECPP_VULKANUTIL_H
