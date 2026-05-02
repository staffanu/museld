//
// Created by staffanu on 9/25/23.
//

#ifndef MUSECPP_TIMESTAMPQUERYPOOL_H
#define MUSECPP_TIMESTAMPQUERYPOOL_H

#include <vector>
#include <string>
#include <vulkan/vulkan.hpp>

namespace musevk {
    class CommandBuffer;
    class TimestampQueryPool {
    public:
        TimestampQueryPool(vk::PhysicalDevice &physical_device,
                           vk::Device &device,
                           uint32_t max_timestamps);
        TimestampQueryPool(TimestampQueryPool &other) = delete;
        void operator=(const TimestampQueryPool &) = delete;

        ~TimestampQueryPool();

        void reset(CommandBuffer &command_buffer);
        void timestamp(CommandBuffer &command_buffer, std::string const &label, vk::PipelineStageFlagBits stage);
        std::vector<std::pair<std::string, int>> getTimestamps();

    private:
        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        unsigned int m_allocated_timestamp_queries;
        vk::QueryPool m_timestamp_query_pool;
        std::vector<std::string> m_timestamped_operations;
    };
}

#endif //MUSECPP_TIMESTAMPQUERYPOOL_H
