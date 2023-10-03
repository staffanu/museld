//
// Created by staffanu on 5/25/23.
//

#ifndef MUSECPP_COMMANDBUFFER_H
#define MUSECPP_COMMANDBUFFER_H

#include <vulkan/vulkan.hpp>
#include "ComputeShader.h"

namespace musevk {
    class TimestampQueryPool;
    class VulkanBuffer;

    class CommandBuffer {
    public:
        CommandBuffer(CommandBuffer &other) = delete;
        void operator=(const CommandBuffer &) = delete;

        CommandBuffer(vk::CommandPool &command_pool,
                      vk::Device &device,
                      vk::Queue &computeQueue,
                      TimestampQueryPool *timestamp_query_pool);

        vk::CommandBuffer getCommandBuffer() { return m_command_buffer; }

        void begin();

        void enqueueTransitionMemoryLayout(vk::Image image,
                                           vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                                           vk::PipelineStageFlagBits srcStageMask, vk::PipelineStageFlagBits dstStageMask,
                                           vk::AccessFlags srcAccessMask, vk::AccessFlags dstAccessMask);

        void enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination);

        void enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                      vk::Image imageTo,
                                      vk::ImageLayout layout,
                                      vk::BufferImageCopy region);

        void enqueueBlitImage(vk::Image &source,
                              vk::ImageLayout source_layout,
                              vk::Image &dest,
                              vk::ImageLayout dest_layout,
                              vk::ImageBlit region);

        void enqueueMemoryBarrier(VulkanBuffer &buffer,
                vk::AccessFlagBits srcAccessMask,
                vk::AccessFlagBits dstAccessMask,
                vk::PipelineStageFlagBits srcStageMask,
                vk::PipelineStageFlagBits dstStageMask);

        template<typename T = float>
        void enqueueComputeShader(const std::shared_ptr<ComputeShader> &compute_shader,
                                  const std::vector<T> &pushConstants,
                                  int descriptor_set_index = 0) {
            for (const std::shared_ptr<VulkanMemoryObject> &buffer: compute_shader->getMemoryObjects(descriptor_set_index)) {
                if (buffer->getType() == eBuffer) {
                    enqueueMemoryBarrier((VulkanBuffer &)(*buffer),
                                         vk::AccessFlagBits::eTransferWrite,
                                         vk::AccessFlagBits::eShaderRead,
                                         vk::PipelineStageFlagBits::eTransfer,
                                         vk::PipelineStageFlagBits::eComputeShader);
                }
            }

            compute_shader->bindPipelineAndDescriptorSets(m_command_buffer, descriptor_set_index);
            compute_shader->bindPushConstants(m_command_buffer, pushConstants);
            compute_shader->dispatch(m_command_buffer);
            maybeTimestamp(compute_shader->name(), vk::PipelineStageFlagBits::eComputeShader);
        }

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
        vk::Queue &m_compute_queue;
        TimestampQueryPool *m_timestamp_query_pool;
        vk::CommandBuffer m_command_buffer;

        vk::Fence m_fence;

        bool m_recording = false;
        bool m_is_running = false;
    };
}

#endif //MUSECPP_COMMANDBUFFER_H
