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
                     uint32_t totalTimestamps = 0);

        void enqueueTransitionMemoryLayout(vk::Image image, vk::Format format,
                                           vk::ImageLayout oldLayout, vk::ImageLayout newLayout);

        void enqueueCopyBuffer(VulkanBuffer &source, VulkanBuffer &destination);

        void enqueueCopyBufferToImage(vk::Buffer &bufferFrom,
                                      vk::Image imageTo,
                                      vk::ImageLayout layout,
                                      vk::BufferImageCopy region);

        template<typename T = float>
        void enqueueComputeShader(const std::shared_ptr<ComputeShader> &compute_shader,
                                  const std::vector<T> &pushConstants,
                                  int descriptor_set_index = 0) {
            m_operation_count++;
            this->begin();
            for (const std::shared_ptr<VulkanBuffer> &buffer: compute_shader->getBuffers(descriptor_set_index)) {
                buffer->enqueueMemoryBarrier(
                        m_command_buffer,
                        vk::AccessFlagBits::eTransferWrite,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eComputeShader);
            }

            compute_shader->enqueueBindCore(m_command_buffer, descriptor_set_index);
            compute_shader->enqueueBindPush(m_command_buffer, pushConstants);
            compute_shader->enqueueDispatch(m_command_buffer);
        }

        void evalAsync() {
            if (m_recording)
                this->end();
            assert(!m_is_running);
            m_is_running = true;

            vk::SubmitInfo submitInfo(m_wait_semaphores, m_wait_dst_stage_masks, m_command_buffer);
            m_fence = m_device.createFence(vk::FenceCreateInfo());
            m_compute_queue.submit(submitInfo, m_fence);
        }

        void evalAwait() {
            assert(m_is_running);
            auto result = m_device.waitForFences(m_fence, VK_TRUE, UINT64_MAX);
            assert(result != vk::Result::eTimeout);
            m_device.destroy(m_fence);
            m_is_running = false;
        }

        void clear() {
            if (m_recording)
                this->end();
        }

        std::vector<std::uint64_t> getTimestamps();

        void begin() {
            if (m_recording) {
                return;
            }

            assert(!m_is_running);
            this->m_command_buffer.begin(vk::CommandBufferBeginInfo());
            this->m_recording = true;

            // latch the first timestamp before any commands are submitted
            if (m_free_timestamp_query_pool)
                m_command_buffer.writeTimestamp(
                        vk::PipelineStageFlagBits::eAllCommands,
                        m_timestampQueryPool,
                        0);
        }

        void end() {
            assert(!m_is_running);
            assert(m_recording);
            m_command_buffer.end();
            m_recording = false;
        }

        ~CommandQueue();

    private:
        void createCommandPool();
        void createCommandBuffer();
        void createTimestampQueryPool(uint32_t total_timestamps);

        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        vk::Queue &m_compute_queue;
        uint32_t m_queue_index;

        vk::CommandPool m_command_pool;
        vk::CommandBuffer m_command_buffer;

        vk::Fence m_fence;
        int m_operation_count = 0;
        vk::QueryPool m_timestampQueryPool;
        bool m_free_timestamp_query_pool = false;

        std::vector<vk::Semaphore> m_wait_semaphores;
        std::vector<vk::PipelineStageFlags> m_wait_dst_stage_masks;

        bool m_recording = false;
        bool m_is_running = false;
    };
}

#endif //MUSECPP_COMMANDQUEUE_H
