//
// Created by staffanu on 5/25/23.
//

#ifndef MUSECPP_COMMANDQUEUE_H
#define MUSECPP_COMMANDQUEUE_H

#include <vulkan/vulkan.hpp>
#include "ComputeShader.h"

namespace musevk {
    class CommandQueue {
    public:
        CommandQueue(CommandQueue &other) = delete;
        void operator=(const CommandQueue &) = delete;

        CommandQueue(vk::PhysicalDevice &physicalDevice,
                     vk::Device &device,
                     vk::Queue &computeQueue,
                     uint32_t queueIndex,
                     std::vector<vk::Semaphore> &wait_semaphores,
                     std::vector<vk::PipelineStageFlags> &wait_dst_stage_masks,
                     uint32_t max_timestamps = 0);

        void enqueueTransitionMemoryLayout(vk::Image image, vk::Format format,
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
            begin();
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

        void maybeTimestamp(std::string label, vk::PipelineStageFlagBits stage);

        void evalAsync();

        void evalAwait();

        std::vector<std::pair<std::string, int>> getTimestamps();

        ~CommandQueue();

    private:
        void begin();

        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        vk::Queue &m_compute_queue;
        uint32_t m_queue_index;

        vk::CommandPool m_command_pool;
        vk::CommandBuffer m_command_buffer;

        vk::Fence m_fence;

        int m_allocated_timestamp_queries;
        vk::QueryPool m_timestamp_query_pool;
        std::vector<std::string> m_timestamped_operations;

        std::vector<vk::Semaphore> m_wait_semaphores;
        std::vector<vk::PipelineStageFlags> m_wait_dst_stage_masks;

        bool m_recording = false;
        bool m_is_running = false;
    };
}

#endif //MUSECPP_COMMANDQUEUE_H
