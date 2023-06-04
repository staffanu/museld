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
                               std::vector<vk::PipelineStageFlags> &wait_dst_stage_masks,
                               uint32_t totalTimestamps)
            : m_physical_device(physicalDevice),
              m_device(device),
              m_compute_queue(computeQueue),
              m_queue_index(queueIndex),
              m_wait_semaphores(wait_semaphores),
              m_wait_dst_stage_masks(wait_dst_stage_masks) {

        createCommandPool();
        createCommandBuffer();
        if (totalTimestamps > 0)
            createTimestampQueryPool(totalTimestamps + 1); //+1 for the first one
    }

    CommandQueue::~CommandQueue() {
        m_device.freeCommandBuffers(m_command_pool, m_command_buffer);
        m_device.destroy(m_command_pool);
        if (m_timestampQueryPool) {
            m_device.destroy(m_timestampQueryPool);
        }
    }

    void CommandQueue::enqueueTransitionMemoryLayout(vk::Image image, vk::Format format,
                                                     vk::ImageLayout oldLayout, vk::ImageLayout newLayout) {
        m_operation_count++;
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
    }

    void CommandQueue::enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination) {
        this->begin();
        m_operation_count++;
        vk::DeviceSize buffer_size(source.getMemorySize());
        vk::BufferCopy copy_region(0, 0, buffer_size);
        m_command_buffer.copyBuffer(source.buffer(), destination.buffer(), copy_region);
    }

    void CommandQueue::enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                  vk::Image imageTo,
                                  vk::ImageLayout layout,
                                  vk::BufferImageCopy region) {
        this->begin();
        m_operation_count++;
        m_command_buffer.copyBufferToImage(bufferFrom, imageTo, layout, region);
    }

    std::vector<std::uint64_t> CommandQueue::getTimestamps() {
        const auto n = m_operation_count + 1;
        std::vector<std::uint64_t> timestamps(n, 0);
        auto result = m_device.getQueryPoolResults(
                m_timestampQueryPool,
                0,
                n,
                timestamps.size() * sizeof(std::uint64_t),
                timestamps.data(),
                sizeof(uint64_t),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("error");

        return timestamps;
    }

    void CommandQueue::createCommandPool() {
        vk::CommandPoolCreateInfo commandPoolInfo(vk::CommandPoolCreateFlags(), m_queue_index);
        m_command_pool = m_device.createCommandPool(commandPoolInfo);
    }

    void CommandQueue::createCommandBuffer() {
        vk::CommandBufferAllocateInfo commandBufferAllocateInfo(m_command_pool, vk::CommandBufferLevel::ePrimary, 1);
        m_command_buffer = m_device.allocateCommandBuffers(commandBufferAllocateInfo)[0];
    }

    void CommandQueue::createTimestampQueryPool(uint32_t total_timestamps) {
        vk::PhysicalDeviceProperties physicalDeviceProperties = m_physical_device.getProperties();

        m_free_timestamp_query_pool = true;
        if (physicalDeviceProperties.limits.timestampComputeAndGraphics) {
            vk::QueryPoolCreateInfo query_pool_info;
            query_pool_info.setQueryCount(total_timestamps);
            query_pool_info.setQueryType(vk::QueryType::eTimestamp);
            m_timestampQueryPool = m_device.createQueryPool(query_pool_info);
        } else {
            throw std::runtime_error("Device does not support timestamps");
        }
    }
}
