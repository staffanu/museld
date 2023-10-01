//
// Created by staffanu on 5/25/23.
//

#include "CommandBuffer.h"
#include "TimestampQueryPool.h"

namespace musevk {
    CommandBuffer::CommandBuffer(vk::CommandPool &command_pool,
                                 vk::Device &device,
                                 vk::Queue &computeQueue,
                                 TimestampQueryPool *timestamp_query_pool)
            : m_command_pool(command_pool),
              m_device(device),
              m_compute_queue(computeQueue),
              m_timestamp_query_pool(timestamp_query_pool) {

        vk::CommandBufferAllocateInfo commandBufferAllocateInfo(m_command_pool, vk::CommandBufferLevel::ePrimary, 1);
        m_command_buffer = m_device.allocateCommandBuffers(commandBufferAllocateInfo)[0];
        m_fence = m_device.createFence(vk::FenceCreateInfo());
    }

    CommandBuffer::~CommandBuffer() {
        m_device.destroy(m_fence);
        m_device.freeCommandBuffers(m_command_pool, m_command_buffer);
    }

    void CommandBuffer::enqueueTransitionMemoryLayout(vk::Image image, vk::Format format,
                                                      vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                                                      vk::PipelineStageFlagBits srcStage, vk::PipelineStageFlagBits dstStage,
                                                      vk::AccessFlags srcAccessMask, vk::AccessFlags dstAccessMask) {
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
        barrier.srcAccessMask = srcAccessMask;
        barrier.dstAccessMask = dstAccessMask;
        m_command_buffer.pipelineBarrier(srcStage,
                                         dstStage,
                                         vk::DependencyFlags(0),
                                         {},
                                         {},
                                         {barrier});
        maybeTimestamp("transitionImage", dstStage);
    }

    void CommandBuffer::enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination) {
        vk::DeviceSize buffer_size(source.getMemorySize());
        vk::BufferCopy copy_region(0, 0, buffer_size);
        m_command_buffer.copyBuffer(source.buffer(), destination.buffer(), copy_region);
        maybeTimestamp("copy", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                                 vk::Image imageTo,
                                                 vk::ImageLayout layout,
                                                 vk::BufferImageCopy region) {
        m_command_buffer.copyBufferToImage(bufferFrom, imageTo, layout, region);
        maybeTimestamp("copyToImage", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueBlitImage(vk::Image &source, vk::ImageLayout source_layout,
                                         vk::Image &dest, vk::ImageLayout dest_layout,
                                         vk::ImageBlit region) {
        m_command_buffer.blitImage(source, source_layout, dest, dest_layout, region, vk::Filter::eLinear);
    }

    void CommandBuffer::enqueueMemoryBarrier(VulkanBuffer &buffer,
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

    void CommandBuffer::maybeTimestamp(std::string const &label, vk::PipelineStageFlagBits stage) {
        if (m_timestamp_query_pool != nullptr)
            m_timestamp_query_pool->timestamp(*this, label, stage);
    }

    void CommandBuffer::begin() {
        assert(!m_is_running);
        assert(!m_recording);
        m_command_buffer.begin(vk::CommandBufferBeginInfo());
        m_recording = true;
    }

    void CommandBuffer::submit(std::vector<vk::Semaphore> const &wait_semaphores,
                               std::vector<vk::PipelineStageFlags> const &wait_dst_stage_masks,
                               std::vector<vk::Semaphore> const &signal_semaphores) {
        assert(!m_is_running);
        assert(m_recording);
        m_command_buffer.end();
        m_recording = false;
        m_is_running = true;

        vk::SubmitInfo submitInfo(wait_semaphores, wait_dst_stage_masks, m_command_buffer, signal_semaphores);
        m_compute_queue.submit(submitInfo, m_fence);
    }

    bool CommandBuffer::isSubmitted() const {
        return m_is_running;
    }

    void CommandBuffer::wait() {
        assert(m_is_running);
        auto result = m_device.waitForFences(m_fence, VK_TRUE, UINT64_MAX);
        assert(result == vk::Result::eSuccess);
        m_device.resetFences(m_fence);
        m_is_running = false;
    }
}
