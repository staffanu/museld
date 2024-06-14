//
// Created by staffanu on 5/25/23.
//

#include <fmt/format.h>
#include "CommandBuffer.h"
#include "VulkanBuffer.h"
#include "TimestampQueryPool.h"

namespace musevk {
    CommandBuffer::CommandBuffer(vk::CommandPool &command_pool,
                                 vk::Device &device,
                                 Queue &queue,
                                 TimestampQueryPool *timestamp_query_pool,
                                 const vk::PhysicalDeviceLimits &limits)
            : m_command_pool(command_pool),
              m_device(device),
              m_queue(queue),
              m_timestamp_query_pool(timestamp_query_pool),
              m_limits(limits){

        vk::CommandBufferAllocateInfo commandBufferAllocateInfo(m_command_pool, vk::CommandBufferLevel::ePrimary, 1);
        m_command_buffer = m_device.allocateCommandBuffers(commandBufferAllocateInfo)[0];
        m_fence = m_device.createFence(vk::FenceCreateInfo());
    }

    CommandBuffer::~CommandBuffer() {
        m_device.destroy(m_fence);
        m_device.freeCommandBuffers(m_command_pool, m_command_buffer);
    }

    void CommandBuffer::enqueueTransitionMemoryLayout(vk::Image image,
                                                      vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                                      vk::PipelineStageFlagBits src_stage, vk::PipelineStageFlagBits dst_stage,
                                                      vk::AccessFlags src_access_mask, vk::AccessFlags dst_access_mask) {
        assert(m_recording);
        vk::ImageMemoryBarrier barrier;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = src_access_mask;
        barrier.dstAccessMask = dst_access_mask;
        m_command_buffer.pipelineBarrier(src_stage,
                                         dst_stage,
                                         vk::DependencyFlags(0),
                                         {},
                                         {},
                                         {barrier});
        maybeTimestamp("transitionImage", dst_stage);
    }

    void CommandBuffer::enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination) {
        vk::DeviceSize buffer_size(source.getMemorySize());
        enqueueCopyBuffer(source, destination, 0, 0, buffer_size);
    }

    void CommandBuffer::enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &dest, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) {
        assert(m_recording);
        vk::BufferCopy region(srcOffset, dstOffset, size);
        m_command_buffer.copyBuffer(source.buffer(), dest.buffer(), {region});
        maybeTimestamp("copy", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueCopyRawBuffer(vk::Buffer &source, vk::Buffer &dest, uint32_t srcOffset, uint32_t dstOffset, uint32_t size) {
        assert(m_recording);
        vk::BufferCopy region(srcOffset, dstOffset, size);
        m_command_buffer.copyBuffer(source, dest, {region});
        maybeTimestamp("raw copy", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                                 vk::Image imageTo,
                                                 vk::ImageLayout layout,
                                                 vk::BufferImageCopy region) {
        assert(m_recording);
        m_command_buffer.copyBufferToImage(bufferFrom, imageTo, layout, region);
        maybeTimestamp("copyToImage", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueCopyImageToBuffer(vk::Image imageFrom,
                                                 vk::ImageLayout layout,
                                                 vk::Buffer &bufferTo,
                                                 vk::BufferImageCopy region) {
        assert(m_recording);
        m_command_buffer.copyImageToBuffer(imageFrom, layout, bufferTo, region);
        maybeTimestamp("copyFromImage", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueBlitImage(vk::Image &source, vk::ImageLayout source_layout,
                                         vk::Image &dest, vk::ImageLayout dest_layout,
                                         vk::ImageBlit region) {
        assert(m_recording);
        m_command_buffer.blitImage(source, source_layout, dest, dest_layout, region, vk::Filter::eLinear);
        maybeTimestamp("blit", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueFillBuffer(VulkanBuffer &buffer, uint32_t data) {
        assert(m_recording);
        m_command_buffer.fillBuffer(buffer.buffer(), 0, VK_WHOLE_SIZE, data);
        maybeTimestamp("fill", vk::PipelineStageFlagBits::eTransfer);
    }

    void CommandBuffer::enqueueBufferBarrier(VulkanBuffer &buffer,
                                             vk::AccessFlagBits srcAccessMask,
                                             vk::AccessFlagBits dstAccessMask,
                                             vk::PipelineStageFlagBits srcStageMask,
                                             vk::PipelineStageFlagBits dstStageMask) {
        assert(m_recording);
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

    void CommandBuffer::enqueueBarrier(vk::AccessFlagBits srcAccessMask,
                                       vk::AccessFlagBits dstAccessMask,
                                       vk::PipelineStageFlagBits srcStageMask,
                                       vk::PipelineStageFlagBits dstStageMask) {
        assert(m_recording);
        vk::MemoryBarrier memoryBarrier;
        memoryBarrier.srcAccessMask = srcAccessMask;
        memoryBarrier.dstAccessMask = dstAccessMask;

        m_command_buffer.pipelineBarrier(srcStageMask,
                                         dstStageMask,
                                         vk::DependencyFlags(),
                                         memoryBarrier,
                                         nullptr,
                                         nullptr);
    }

    void CommandBuffer::invalidateMappedMemoryRangeLater(vk::MappedMemoryRange range) {
        m_memory_ranges_to_invalidate.push_back(range);
    }

    void CommandBuffer::enqueueResetQueryPool(vk::QueryPool const &pool, unsigned int first_query, unsigned int query_count) {
        assert(m_recording);
        m_command_buffer.resetQueryPool(pool, first_query, query_count);
    }

    void CommandBuffer::enqueueWriteTimestamp(vk::PipelineStageFlagBits stage, vk::QueryPool const &pool, unsigned int query) {
        assert(m_recording);
        m_command_buffer.writeTimestamp(stage, pool, query);
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
        m_queue.submit(submitInfo, m_fence);
    }

    bool CommandBuffer::isSubmitted() const {
        return m_is_running;
    }

    void CommandBuffer::wait() {
        assert(m_is_running);
        auto result = m_device.waitForFences(m_fence, VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess)
            throw std::runtime_error(fmt::format("waitForFences returned with result {}", (uint32_t)result));
        m_device.resetFences(m_fence);
        m_is_running = false;
        if (!m_memory_ranges_to_invalidate.empty()) {
            m_device.invalidateMappedMemoryRanges({m_memory_ranges_to_invalidate});
            m_memory_ranges_to_invalidate.clear();
        }
    }
}
