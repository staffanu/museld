// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_COMMANDPOOL_H
#define MUSECPP_COMMANDPOOL_H

#include <cstdint>
#include "CommandBuffer.h"
#include "VulkanManager.h"

namespace musevk {

    class CommandPool {
    public:
        CommandPool(VulkanManager &vulkan_manager)
        : m_vulkan_manager(vulkan_manager) {

            uint32_t queue_family_index = m_vulkan_manager.getGraphicsAndComputeFamily();

            vk::CommandPoolCreateInfo command_pool_info(vk::CommandPoolCreateFlags(
                    vk::CommandPoolCreateFlagBits::eResetCommandBuffer), queue_family_index);

            m_command_pool = m_vulkan_manager.getDevice().createCommandPool(command_pool_info);
        }
        CommandPool(CommandPool &other) = delete;
        CommandPool &operator=(const CommandPool &other) = delete;

        std::shared_ptr<CommandBuffer> createCommandBuffer(
                TimestampQueryPool *timestamp_query_pool = nullptr) {
            return std::make_unique<CommandBuffer>(
                    m_command_pool,
                    m_vulkan_manager.getDevice(),
                    m_vulkan_manager.getGraphicsAndComputeQueue(),
                    timestamp_query_pool,
                    m_vulkan_manager.getPhysicalDeviceProperties().limits);
        }

        ~CommandPool() {
            m_vulkan_manager.getDevice().destroy(m_command_pool);
        }

    private:
        VulkanManager &m_vulkan_manager;
        vk::CommandPool m_command_pool;
    };
}

#endif //MUSECPP_COMMANDPOOL_H
