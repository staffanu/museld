// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include "TimestampQueryPool.h"
#include "CommandBuffer.h"

namespace musevk {
    TimestampQueryPool::TimestampQueryPool(vk::PhysicalDevice &physical_device,
    vk::Device &device,
            uint32_t max_timestamps) :
    m_physical_device(physical_device),
    m_device(device) {

        vk::PhysicalDeviceProperties physical_device_properties = m_physical_device.getProperties();
        if (physical_device_properties.limits.timestampComputeAndGraphics) {
            vk::QueryPoolCreateInfo query_pool_info;
            query_pool_info.setQueryCount(max_timestamps);
            query_pool_info.setQueryType(vk::QueryType::eTimestamp);
            m_timestamp_query_pool = device.createQueryPool(query_pool_info);
        } else {
            throw std::runtime_error("Device does not support timestamps");
        }

        m_allocated_timestamp_queries = max_timestamps;
    }

    TimestampQueryPool::~TimestampQueryPool() {
        if (m_timestamp_query_pool) {
            m_device.destroy(m_timestamp_query_pool);
        }
    }

    void TimestampQueryPool::reset(CommandBuffer &command_buffer) {
        command_buffer.enqueueResetQueryPool(m_timestamp_query_pool, 0, m_allocated_timestamp_queries);
        m_timestamped_operations.clear();
        timestamp(command_buffer, "begin", vk::PipelineStageFlagBits::eTopOfPipe);
    }

    void TimestampQueryPool::timestamp(CommandBuffer &command_buffer, std::string const &label, vk::PipelineStageFlagBits stage) {
        if (m_allocated_timestamp_queries > m_timestamped_operations.size()) {
            command_buffer.enqueueWriteTimestamp(stage, m_timestamp_query_pool, m_timestamped_operations.size());
            m_timestamped_operations.emplace_back(label);
        }
    }

    std::vector<std::pair<std::string, int>> TimestampQueryPool::getTimestamps() {
        assert(m_allocated_timestamp_queries);
        const auto n = m_timestamped_operations.size();
        std::vector<uint64_t> timestamps(n, 0);
        auto result = m_device.getQueryPoolResults(
                m_timestamp_query_pool,
                0,
                n,
                timestamps.size() * sizeof(std::uint64_t),
                timestamps.data(),
                sizeof(uint64_t),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("getQueryPoolResults error");

        vk::PhysicalDeviceProperties physical_device_properties = m_physical_device.getProperties();
        auto time_stamp_period = physical_device_properties.limits.timestampPeriod;

        std::vector<std::pair<std::string, int>> labeled_timestamps(n);
        for (int i = 0; i < n; i++)
            labeled_timestamps[i] = std::pair(m_timestamped_operations[i],
                                              time_stamp_period * (double) (timestamps[i] - timestamps[0]) / 1000);

        return labeled_timestamps;
    }
}
