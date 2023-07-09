//
// Created by staffanu on 5/25/23.
//

#include "CommandQueue.h"

namespace musevk {
    CommandQueue::CommandQueue(vk::PhysicalDevice &physicalDevice,
                               vk::Device &device,
                               vk::Queue &computeQueue,
                               uint32_t queueIndex,
                               std::vector<vk::Semaphore> &wait_semaphores,
                               vk::PipelineStageFlags &wait_dst_stage_masks,
                               uint32_t max_timestamps)
            : m_physical_device(physicalDevice),
              m_device(device),
              m_compute_queue(computeQueue),
              m_queue_index(queueIndex),
              m_wait_semaphores(wait_semaphores),
              m_wait_dst_stage_masks(wait_dst_stage_masks) {

        vk::CommandPoolCreateInfo commandPoolInfo(vk::CommandPoolCreateFlags(
                vk::CommandPoolCreateFlagBits::eResetCommandBuffer), m_queue_index);
        m_command_pool = m_device.createCommandPool(commandPoolInfo);

        vk::CommandBufferAllocateInfo commandBufferAllocateInfo(m_command_pool, vk::CommandBufferLevel::ePrimary, 1);
        m_command_buffer = m_device.allocateCommandBuffers(commandBufferAllocateInfo)[0];

        if (max_timestamps > 0) {
            vk::PhysicalDeviceProperties physical_device_properties = m_physical_device.getProperties();
            if (physical_device_properties.limits.timestampComputeAndGraphics) {
                vk::QueryPoolCreateInfo query_pool_info;
                query_pool_info.setQueryCount(max_timestamps);
                query_pool_info.setQueryType(vk::QueryType::eTimestamp);
                m_timestamp_query_pool = m_device.createQueryPool(query_pool_info);
            } else {
                throw std::runtime_error("Device does not support timestamps");
            }
        }
        m_allocated_timestamp_queries = max_timestamps;
    }

    CommandQueue::~CommandQueue() {
        m_device.freeCommandBuffers(m_command_pool, m_command_buffer);
        m_device.destroy(m_command_pool);
        if (m_timestamp_query_pool) {
            m_device.destroy(m_timestamp_query_pool);
        }
    }

    void CommandQueue::enqueueTransitionMemoryLayout(vk::Image image, vk::Format format,
                                                     vk::ImageLayout oldLayout, vk::ImageLayout newLayout) {
        begin();
        vk::ImageMemoryBarrier barrier;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = vk::AccessFlags(0);
        barrier.dstAccessMask = vk::AccessFlags(vk::AccessFlagBits::eTransferWrite);
        m_command_buffer.pipelineBarrier(vk::PipelineStageFlags(vk::PipelineStageFlagBits::eTopOfPipe),
                                         vk::PipelineStageFlags(vk::PipelineStageFlagBits::eTransfer),
                                         vk::DependencyFlags(0),
                                         {},
                                         {},
                                         {barrier});
        maybeTimestamp("transitionImage");
    }

    void CommandQueue::enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination) {
        begin();
        vk::DeviceSize buffer_size(source.getMemorySize());
        vk::BufferCopy copy_region(0, 0, buffer_size);
        m_command_buffer.copyBuffer(source.buffer(), destination.buffer(), copy_region);
        maybeTimestamp("copy");
    }

    void CommandQueue::enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                                vk::Image imageTo,
                                                vk::ImageLayout layout,
                                                vk::BufferImageCopy region) {
        begin();
        m_command_buffer.copyBufferToImage(bufferFrom, imageTo, layout, region);
        maybeTimestamp("copyToImage");
    }

    void CommandQueue::enqueueBlitImage(vk::Image &source, vk::ImageLayout source_layout,
                                        vk::Image &dest, vk::ImageLayout dest_layout,
                                        vk::ImageBlit region) {
        begin();
        m_command_buffer.blitImage(source, source_layout, dest, dest_layout, region, vk::Filter::eLinear);
    }

    void CommandQueue::enqueueMemoryBarrier(VulkanBuffer &buffer,
                                            vk::AccessFlagBits srcAccessMask,
                                            vk::AccessFlagBits dstAccessMask,
                                            vk::PipelineStageFlagBits srcStageMask,
                                            vk::PipelineStageFlagBits dstStageMask) {
        vk::BufferMemoryBarrier bufferMemoryBarrier;
        bufferMemoryBarrier.buffer = buffer.buffer();
        bufferMemoryBarrier.size = buffer.getMemorySize();
        bufferMemoryBarrier.srcAccessMask = srcAccessMask;
        bufferMemoryBarrier.dstAccessMask = dstAccessMask;
        bufferMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        m_command_buffer.pipelineBarrier(srcStageMask,
                                         dstStageMask,
                                         vk::DependencyFlags(),
                                         nullptr,
                                         bufferMemoryBarrier,
                                         nullptr);
    }

    void CommandQueue::maybeTimestamp(std::string label) {
        if (m_allocated_timestamp_queries > m_timestamped_operations.size()) {
            m_command_buffer.writeTimestamp(
                    vk::PipelineStageFlagBits::eAllCommands,
                    m_timestamp_query_pool,
                    m_timestamped_operations.size());
            m_timestamped_operations.emplace_back(label);
        }
    }

    void CommandQueue::evalAsync() {
        assert(!m_is_running);
        assert(m_recording);
        m_command_buffer.end();
        m_recording = false;
        m_is_running = true;

        vk::SubmitInfo submitInfo(m_wait_semaphores, m_wait_dst_stage_masks, m_command_buffer);
        m_fence = m_device.createFence(vk::FenceCreateInfo());
        m_compute_queue.submit(submitInfo, m_fence);
    }

    void CommandQueue::evalAwait() {
        assert(m_is_running);
        auto result = m_device.waitForFences(m_fence, VK_TRUE, UINT64_MAX);
        assert(result != vk::Result::eTimeout);
        m_device.destroy(m_fence);
        m_is_running = false;
    }

    void CommandQueue::begin() {
        assert(!m_is_running);
        if (!m_recording) {
            m_command_buffer.begin(vk::CommandBufferBeginInfo());
            m_recording = true;

            if (m_allocated_timestamp_queries) {
                m_command_buffer.resetQueryPool(m_timestamp_query_pool, 0, m_allocated_timestamp_queries);
                m_timestamped_operations.clear();
                maybeTimestamp("begin");
            }
        }
    }

    std::vector<std::pair<std::string, int>> CommandQueue::getTimestamps() {
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
                                            time_stamp_period * (double)(timestamps[i] - timestamps[0]) / 1000);

        return labeled_timestamps;
    }
}
