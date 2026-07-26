// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_COMMANDBUFFER_H
#define MUSECPP_COMMANDBUFFER_H

#include <cstdint>
#include <vulkan/vulkan.hpp>
#include "ComputeShader.h"
#include "Queue.h"

namespace musevk {
    class TimestampQueryPool;
    class VulkanBuffer;

    class CommandBuffer {
    public:
        CommandBuffer(CommandBuffer &other) = delete;
        void operator=(const CommandBuffer &) = delete;

        CommandBuffer(vk::CommandPool &command_pool,
                      vk::Device &device,
                      Queue &queue,
                      TimestampQueryPool *timestamp_query_pool,
                      const vk::PhysicalDeviceLimits &limits);

        void begin();

        void enqueueTransitionMemoryLayout(vk::Image image,
                                           vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                           vk::PipelineStageFlagBits src_stage, vk::PipelineStageFlagBits dst_stage,
                                           vk::AccessFlags src_access_mask, vk::AccessFlags dst_access_mask);

        void enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination);

        void enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &dest, uint32_t srcOffset, uint32_t dstOffset, uint32_t size);

        void enqueueCopyRawBuffer(vk::Buffer &source, vk::Buffer &dest, uint32_t srcOffset, uint32_t dstOffset, uint32_t size);

        void enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                      vk::Image imageTo,
                                      vk::ImageLayout layout,
                                      vk::BufferImageCopy region);

        void enqueueCopyImageToBuffer(vk::Image imageFrom,
                                      vk::ImageLayout layout,
                                      vk::Buffer &bufferTo,
                                      vk::BufferImageCopy region);

        void enqueueBlitImage(vk::Image &source,
                              vk::ImageLayout source_layout,
                              vk::Image &dest,
                              vk::ImageLayout dest_layout,
                              vk::ImageBlit region);

        void enqueueFillBuffer(VulkanBuffer &buffer, uint32_t data);

        void enqueueBufferBarrier(VulkanBuffer &buffer,
                vk::AccessFlagBits srcAccessMask,
                vk::AccessFlagBits dstAccessMask,
                vk::PipelineStageFlagBits srcStageMask,
                vk::PipelineStageFlagBits dstStageMask);

        void enqueueBarrier(vk::AccessFlagBits srcAccessMask,
                            vk::AccessFlagBits dstAccessMask,
                            vk::PipelineStageFlagBits srcStageMask,
                            vk::PipelineStageFlagBits dstStageMask);

        template<typename T>
        void enqueueComputeShader(const std::shared_ptr<ComputeShader> &compute_shader,
                                  const std::vector<T> &pushConstants,
                                  int descriptor_set_index = 0) {
            enqueueBarrier(vk::AccessFlagBits::eShaderWrite,
                           vk::AccessFlagBits::eShaderRead,
                           vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eComputeShader);

            compute_shader->bindPipelineAndDescriptorSets(m_command_buffer, descriptor_set_index);
            compute_shader->bindPushConstants(m_command_buffer, pushConstants);
            compute_shader->dispatch(m_command_buffer, m_limits);
            maybeTimestamp(compute_shader->name(), vk::PipelineStageFlagBits::eComputeShader);
        }

        // memory ranges that are invalidated before wait() returns
        void invalidateMappedMemoryRangeLater(vk::MappedMemoryRange range);

        // memory ranges that are flushed before submission
        void flushMappedMemoryRangeLater(vk::MappedMemoryRange range);

        void enqueueResetQueryPool(vk::QueryPool const &pool, unsigned int first_query, unsigned int query_count);

        void enqueueWriteTimestamp(vk::PipelineStageFlagBits stage, vk::QueryPool const &pool, unsigned int query);

        void submit(std::vector<vk::Semaphore> const &wait_semaphores,
                    std::vector<vk::PipelineStageFlags> const &wait_dst_stage_masks,
                    std::vector<vk::Semaphore> const &signal_semaphores);
        bool isSubmitted() const;

        void wait();

        ~CommandBuffer();

    private:
        void maybeTimestamp(std::string const &label, vk::PipelineStageFlagBits stage);

        vk::CommandPool &m_command_pool;
        vk::Device &m_device;
        Queue &m_queue;
        TimestampQueryPool *m_timestamp_query_pool;
        const vk::PhysicalDeviceLimits m_limits;
        vk::CommandBuffer m_command_buffer;

        vk::Fence m_fence;

        bool m_recording = false;
        bool m_is_running = false;
        std::vector<vk::MappedMemoryRange> m_memory_ranges_to_invalidate;
        std::vector<vk::MappedMemoryRange> m_memory_ranges_to_flush;
    };
}

#endif //MUSECPP_COMMANDBUFFER_H
