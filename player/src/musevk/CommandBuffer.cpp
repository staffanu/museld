// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <format>
#include <cstdlib>
#include <set>
#include <string>
#include "CommandBuffer.h"
#include "VulkanBuffer.h"
#include "TimestampQueryPool.h"
#include "GpuTimeline.h"

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
        if (GpuTimeline::enabled() && m_limits.timestampComputeAndGraphics) {
            vk::QueryPoolCreateInfo query_pool_info;
            query_pool_info.setQueryCount(2);
            query_pool_info.setQueryType(vk::QueryType::eTimestamp);
            m_timeline_query_pool = m_device.createQueryPool(query_pool_info);
        }
    }

    CommandBuffer::~CommandBuffer() {
        // Never destroy under in-flight work: an unwind between submit() and
        // wait() would otherwise free the fence and buffer mid-execution.  The
        // pointer-style waitForFences overload reports errors as result codes
        // rather than exceptions, and there is nothing to do about them here.
        if (m_is_running)
            (void)m_device.waitForFences(1, &m_fence, VK_TRUE, UINT64_MAX);
        m_device.destroy(m_fence);
        if (m_timeline_query_pool)
            m_device.destroy(m_timeline_query_pool);
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

    void CommandBuffer::enqueueClearColorImage(vk::Image &image, vk::ImageLayout layout, vk::ClearColorValue color) {
        assert(m_recording);
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        m_command_buffer.clearColorImage(image, layout, color, range);
        maybeTimestamp("clear", vk::PipelineStageFlagBits::eTransfer);
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

    void CommandBuffer::flushMappedMemoryRangeLater(vk::MappedMemoryRange range) {
        m_memory_ranges_to_flush.push_back(range);
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
        if (m_timeline_query_pool) {
            m_command_buffer.resetQueryPool(m_timeline_query_pool, 0, 2);
            m_command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_timeline_query_pool, 0);
        }
    }

    void CommandBuffer::submit(std::vector<vk::Semaphore> const &wait_semaphores,
                               std::vector<vk::PipelineStageFlags> const &wait_dst_stage_masks,
                               std::vector<vk::Semaphore> const &signal_semaphores) {
        assert(!m_is_running);
        assert(m_recording);
        if (m_timeline_query_pool)
            m_command_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_timeline_query_pool, 1);
        m_command_buffer.end();
        m_recording = false;
        m_is_running = true;

        if (!m_memory_ranges_to_flush.empty()) {
            m_device.flushMappedMemoryRanges({m_memory_ranges_to_flush});
            m_memory_ranges_to_flush.clear();
        }

        vk::SubmitInfo submitInfo(wait_semaphores, wait_dst_stage_masks, m_command_buffer, signal_semaphores);
        auto result = m_queue.submit(submitInfo, m_fence);
        if (result != vk::Result::eSuccess)
            throw std::runtime_error(std::format("queue submit returned with result {}", (uint32_t)result));
        if (m_timeline_query_pool)
            m_timeline_submit_ns = GpuTimeline::nowNs();
    }

    bool CommandBuffer::isSubmitted() const {
        return m_is_running;
    }

    void CommandBuffer::wait() {
        assert(m_is_running);
        const int64_t wait_ns = m_timeline_query_pool ? GpuTimeline::nowNs() : 0;
        // It returned a result *and* threw because the enhanced overload does
        // both: the result told eSuccess from eTimeout, its two success codes,
        // while anything else -- eErrorDeviceLost and friends -- came back as an
        // exception. The pointer overload just returns all of them.
        auto result = m_device.waitForFences(1, &m_fence, VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess)
            throw std::runtime_error(std::format("waitForFences returned with result {}", (uint32_t)result));
        if (m_timeline_query_pool) {
            const int64_t done_ns = GpuTimeline::nowNs();
            uint64_t ticks[2] = {0, 0};
            if (m_device.getQueryPoolResults(m_timeline_query_pool, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                             vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait) == vk::Result::eSuccess)
                GpuTimeline::record(m_timeline_label, m_timeline_submit_ns, wait_ns, done_ns, ticks[0], ticks[1],
                                    m_limits.timestampPeriod);
        }
        m_device.resetFences(m_fence);
        m_is_running = false;
        if (!m_memory_ranges_to_invalidate.empty()) {
            m_device.invalidateMappedMemoryRanges({m_memory_ranges_to_invalidate});
            m_memory_ranges_to_invalidate.clear();
        }
    }
}

namespace musevk {
    bool CommandBuffer::passSkipped(const std::string &name) {
        static const std::set<std::string> skipped = [] {
            std::set<std::string> names;
            if (const char *env = getenv("MUSELD_SKIP_PASS")) {
                std::string list(env);
                size_t start = 0;
                while (start <= list.size()) {
                    const size_t comma = list.find(',', start);
                    const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!item.empty())
                        names.insert(item);
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
            }
            return names;
        }();
        return !skipped.empty() && skipped.count(name) != 0;
    }
}
