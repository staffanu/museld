//
// Created by staffanu on 5/25/23.
//

#ifndef MUSECPP_VULKANRESOURCES_H
#define MUSECPP_VULKANRESOURCES_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <memory>
#include <iostream>
#include <fmt/format.h>
#include "ComputeShader.h"

#define KP_LOG_DEBUG(...)
#define KP_LOG_INFO(...)
#define KP_LOG_WARN(...) std::cout << "WARN " << fmt::format(__VA_ARGS__) << std::endl;

namespace musevk {
    class Sequence {
    public:
        Sequence(Sequence &other) = delete;

        void operator=(const Sequence &) = delete;

        Sequence(vk::PhysicalDevice &physicalDevice,
                 vk::Device &device,
                 vk::Queue &computeQueue,
                 uint32_t queueIndex,
                 std::vector<vk::Semaphore> wait_semaphores,
                 std::vector<vk::PipelineStageFlags> wait_dst_stage_masks,
                 uint32_t totalTimestamps = 0)
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

        void recordTransionMemoryLayout(vk::Image image, vk::Format format,
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

        void recordCopyBufferToImage(std::shared_ptr<vk::Buffer> bufferFrom,
                                     vk::Image imageTo,
                                     vk::ImageLayout layout,
                                     vk::BufferImageCopy region) {
            this->begin();
            m_operation_count++;
            m_command_buffer.copyBufferToImage(*bufferFrom, imageTo, layout, region);
        }

        void recordSyncLocal(std::shared_ptr<Tensor> tensor) {
            this->begin();
            m_operation_count++;
            if (tensor->tensorType() == Tensor::TensorTypes::eDevice) {

                tensor->recordPrimaryBufferMemoryBarrier(
                        m_command_buffer,
                        vk::AccessFlagBits::eShaderWrite,
                        vk::AccessFlagBits::eTransferRead,
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eTransfer);

                tensor->recordCopyFromDeviceToStaging(m_command_buffer);

                tensor->recordPrimaryBufferMemoryBarrier(
                        m_command_buffer,
                        vk::AccessFlagBits::eTransferWrite,
                        vk::AccessFlagBits::eHostRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eHost);
            }
        }

        void recordSyncDevice(std::shared_ptr<Tensor> tensor) {
            this->begin();
            m_operation_count++;
            if (tensor->tensorType() == Tensor::TensorTypes::eDevice) {
                tensor->recordCopyFromStagingToDevice(m_command_buffer);
            }
        }

        std::vector<std::shared_ptr<ComputeShader>> m_ComputeShaders;

        template<typename T = float>
        void recordComputeShader(const std::shared_ptr<ComputeShader> &compute_shader,
                                 const std::vector<T> &pushConstants) {
            m_ComputeShaders.push_back(compute_shader);
            m_operation_count++;
            this->begin();
            uint32_t mPushConstantsDataTypeMemorySize = 0;
            uint32_t mPushConstantsSize = 0;
            std::vector<float> push_constants;
            for (auto &pc: pushConstants) {
                auto *p = reinterpret_cast<const float *>(&pc);
                push_constants.emplace_back(*p);
            }

            for (const std::shared_ptr<Tensor> &tensor: compute_shader->getTensors()) {
                tensor->recordPrimaryBufferMemoryBarrier(
                        m_command_buffer,
                        vk::AccessFlagBits::eTransferWrite,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eComputeShader);
            }

            if (!push_constants.empty()) {
                compute_shader->setPushConstants(push_constants);
            }

            compute_shader->recordBindCore(m_command_buffer);
            compute_shader->recordBindPush(m_command_buffer);
            compute_shader->recordDispatch(m_command_buffer);
        }

        void evalAsync() {
            if (this->isRecording()) {
                this->end();
            }

            if (this->mIsRunning)
                throw std::runtime_error(
                        "Sequence evalAsync called when an eval async was called without successful wait");

            this->mIsRunning = true;

            vk::SubmitInfo submitInfo(m_wait_semaphores, m_wait_dst_stage_masks, m_command_buffer);
            m_fence = m_device.createFence(vk::FenceCreateInfo());
            m_compute_queue.submit(submitInfo, m_fence);
        }

        void evalAwait() {
            assert(mIsRunning);
            auto result = m_device.waitForFences(m_fence, VK_TRUE, UINT64_MAX);
            assert(result != vk::Result::eTimeout);
            m_device.destroy(m_fence);
            mIsRunning = false;
        }

        void clear() {
            if (this->isRecording()) {
                this->end();
            }
        }

        std::vector<std::uint64_t> getTimestamps() {
            const auto n = m_operation_count + 1;
            std::vector<std::uint64_t> timestamps(n, 0);
            auto result = m_device.getQueryPoolResults(
                    timestampQueryPool,
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

        void begin() {
            if (this->isRecording()) {
                KP_LOG_DEBUG("Kompute Sequence begin called when already recording");
                return;
            }

            if (this->isRunning()) {
                throw std::runtime_error(
                        "Kompute Sequence begin called when sequence still running");
            }

            KP_LOG_INFO("Kompute Sequence command now started recording");
            this->m_command_buffer.begin(vk::CommandBufferBeginInfo());
            this->mRecording = true;

            // latch the first timestamp before any commands are submitted
            if (mFreeTimetampQueryPool)
                m_command_buffer.writeTimestamp(
                        vk::PipelineStageFlagBits::eAllCommands,
                        timestampQueryPool,
                        0);
        }

        void end() {
            if (this->isRunning()) {
                throw std::runtime_error(
                        "Kompute Sequence begin called when sequence still running");
            }

            if (!this->isRecording()) {
                KP_LOG_WARN("Kompute Sequence end called when not recording");
                return;
            } else {
                KP_LOG_INFO("Kompute Sequence command recording END");
                this->m_command_buffer.end();
                this->mRecording = false;
            }
        }

        bool isRecording() const {
            return this->mRecording;
        }

        bool isRunning() const {
            return this->mIsRunning;
        }

        ~Sequence() {
            m_device.freeCommandBuffers(m_command_pool, m_command_buffer);
            m_device.destroy(m_command_pool);

            if (timestampQueryPool) {
                m_device.destroy(timestampQueryPool);
            }
        }

    private:
        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        vk::Queue &m_compute_queue;
        uint32_t m_queue_index;

        vk::CommandPool m_command_pool;
        vk::CommandBuffer m_command_buffer;

        vk::Fence m_fence;
        int m_operation_count = 0;
        vk::QueryPool timestampQueryPool;
        bool mFreeTimetampQueryPool = false;

        std::vector<vk::Semaphore> m_wait_semaphores;
        std::vector<vk::PipelineStageFlags> m_wait_dst_stage_masks;

        bool mRecording = false;
        bool mIsRunning = false;

        void createCommandPool() {
            vk::CommandPoolCreateInfo commandPoolInfo(vk::CommandPoolCreateFlags(), m_queue_index);
            m_command_pool = m_device.createCommandPool(commandPoolInfo);
        }

        void createCommandBuffer() {
            vk::CommandBufferAllocateInfo commandBufferAllocateInfo(m_command_pool, vk::CommandBufferLevel::ePrimary, 1);
            m_command_buffer = m_device.allocateCommandBuffers(commandBufferAllocateInfo)[0];
        }

        void createTimestampQueryPool(uint32_t totalTimestamps) {
            vk::PhysicalDeviceProperties physicalDeviceProperties = m_physical_device.getProperties();

            mFreeTimetampQueryPool = true;
            if (physicalDeviceProperties.limits.timestampComputeAndGraphics) {
                vk::QueryPoolCreateInfo queryPoolInfo;
                queryPoolInfo.setQueryCount(totalTimestamps);
                queryPoolInfo.setQueryType(vk::QueryType::eTimestamp);
                this->timestampQueryPool = m_device.createQueryPool(queryPoolInfo);
            } else {
                throw std::runtime_error("Device does not support timestamps");
            }
        }
    };

    class VulkanResources {
    public:
        VulkanResources(vk::PhysicalDevice &physical_device, vk::Device &device,
                        vk::Queue &computeQueue, uint32_t compute_queue_index)
                : m_physical_device(physical_device), m_device(device),
                  m_compute_queue(computeQueue), m_compute_queue_index(compute_queue_index) {
        }

        VulkanResources(VulkanResources &other) = delete;

        void operator=(const VulkanResources &) = delete;

        std::shared_ptr<Tensor> tensor(const std::vector<float> &data,
                                       Tensor::TensorTypes tensorType = Tensor::TensorTypes::eDevice) {
            return std::make_shared<Tensor>(
                    std::make_shared<vk::PhysicalDevice>(vk::PhysicalDevice(m_physical_device)),
                    std::make_shared<vk::Device>(vk::Device(m_device)),
                    (void *) data.data(), data.size(), sizeof(float), tensorType);
        }

        std::shared_ptr<Tensor> tensor(
                void *data,
                uint32_t elementTotalCount,
                uint32_t elementMemorySize,
                Tensor::TensorTypes tensorType = Tensor::TensorTypes::eDevice) {
            return std::make_shared<Tensor>(
                    std::make_shared<vk::PhysicalDevice>(vk::PhysicalDevice(m_physical_device)),
                    std::make_shared<vk::Device>(vk::Device(m_device)),
                    data, elementTotalCount, elementMemorySize, tensorType);
        }

        std::shared_ptr<Sequence> sequence(
                std::vector<vk::Semaphore> wait_semaphores = {},
                std::vector<vk::PipelineStageFlags> wait_dst_stage_masks = {},
                uint32_t totalTimestamps = 0) {
            return std::make_shared<Sequence>(
                    m_physical_device,
                    m_device,
                    m_compute_queue,
                    m_compute_queue_index,
                    wait_semaphores,
                    wait_dst_stage_masks,
                    totalTimestamps);
        }

        template<typename S = float>
        std::shared_ptr<ComputeShader> ComputeShader(
                const std::vector<std::shared_ptr<Tensor>> &tensors,
                const std::vector<uint32_t> &spirv,
                const Workgroup &workgroup) {
            return std::make_shared<musevk::ComputeShader>(m_device, tensors, spirv, workgroup);
        }

    private:
        vk::PhysicalDevice &m_physical_device;
        vk::Device &m_device;
        vk::Queue &m_compute_queue;
        uint32_t m_compute_queue_index;

    };
}

#endif //MUSECPP_VULKANRESOURCES_H
