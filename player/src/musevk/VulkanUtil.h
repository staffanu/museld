// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

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

        // Used to allocate a device local storage buffer initialized with the given data
        template<typename T>
        static std::unique_ptr<VulkanBuffer> createDeviceBuffer(VulkanManager &vulkan_manager, CommandPool &command_pool, Size const &size, const std::vector<T> &data) {
            assert(size.numberOfElements() == data.size());
            auto buffer = std::make_unique<VulkanBuffer>(vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eStorageBuffer, eHostWriteRarely);
            for (int i = 0; i < data.size(); i++)
                buffer->data<T>()[i] = data[i];

            auto command_buffer = command_pool.createCommandBuffer();
            command_buffer->begin();
            buffer->synchronizeHostWrites(*command_buffer);
            command_buffer->submit({}, {}, {});
            command_buffer->wait();
            return buffer;
        }

        template<typename T, size_t n>
        static std::unique_ptr<VulkanBuffer> createDeviceBuffer(VulkanManager &vulkan_manager, CommandPool &command_pool, Size const &size, const std::array<T, n> &data) {
            assert(size.numberOfElements() == n);
            auto buffer = std::make_unique<VulkanBuffer>(vulkan_manager, size, sizeof(T), vk::BufferUsageFlagBits::eStorageBuffer, eHostWriteRarely);
            for (int i = 0; i < data.size(); i++)
                buffer->data<T>()[i] = data[i];

            auto command_buffer = command_pool.createCommandBuffer();
            command_buffer->begin();
            buffer->synchronizeHostWrites(*command_buffer);
            command_buffer->submit({}, {}, {});
            command_buffer->wait();
            return buffer;
        }

        static std::unique_ptr<VulkanBuffer> createDeviceBufferFloatsAsHalfFloats(VulkanManager &vulkan_manager, CommandPool &command_pool,
                                                                                  Size const &size, const std::vector<float> &data) {
            assert(size.numberOfElements() == data.size());
            std::vector<uint16_t> half_floats(data.size());
            for (int i = 0; i < data.size(); i++)
                half_floats[i] = HalfFloatUtil::float_to_half(data[i]);
            return createDeviceBuffer<uint16_t>(vulkan_manager, command_pool, size, half_floats);
        }
    };
}


#endif //MUSECPP_VULKANUTIL_H
