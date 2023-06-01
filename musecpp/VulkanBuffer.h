//
// Created by staffanu on 5/31/23.
//

#ifndef MUSECPP_VULKANBUFFER_H
#define MUSECPP_VULKANBUFFER_H

#include <vulkan/vulkan.hpp>
#include <memory>
#include <iostream>

namespace musevk {

    class Tensor {
    public:
        enum class TensorTypes {
            eDevice = 0,  ///< Type is device memory, source and destination
            eHost = 1,    ///< Type is host memory, source and destination
            eStorage = 2, ///< Type is Device memory (only)
        };

        static std::string toString(TensorTypes dt) {
            switch (dt) {
                case TensorTypes::eDevice:
                    return "eDevice";
                case TensorTypes::eHost:
                    return "eHost";
                case TensorTypes::eStorage:
                    return "eStorage";
                default:
                    return "unknown";
            }
        }

        Tensor(std::shared_ptr <vk::PhysicalDevice> physicalDevice,
               std::shared_ptr <vk::Device> device,
               void *data,
               uint32_t elementTotalCount,
               uint32_t elementMemorySize,
               const TensorTypes &tensorType = TensorTypes::eDevice) {
            this->mPhysicalDevice = physicalDevice;
            this->mDevice = device;
            this->mTensorType = tensorType;
            this->mSize = elementTotalCount;
            this->mDataTypeMemorySize = elementMemorySize;

            this->allocateMemoryCreateGPUResources();

            if (this->tensorType() != Tensor::TensorTypes::eStorage) {
                this->mapRawData();
                memcpy(this->mRawData, data, this->memorySize());
            }
        }

        Tensor(const Tensor &other) = delete;

        Tensor &operator=(const Tensor &other) = delete;

        Tensor(Tensor &&other) = delete;

        Tensor &operator=(Tensor &&other) = delete;

        ~Tensor() {
            if (this->tensorType() != Tensor::TensorTypes::eStorage) {
                this->unmapRawData();
            }

            if (this->mFreePrimaryBuffer) {
                this->mDevice->destroy(
                        *this->mPrimaryBuffer,
                        (vk::Optional<const vk::AllocationCallbacks>)
                nullptr);
            }

            if (this->mFreeStagingBuffer) {
                this->mDevice->destroy(
                        *this->mStagingBuffer,
                        (vk::Optional<const vk::AllocationCallbacks>)
                nullptr);
            }

            if (this->mFreePrimaryMemory) {
                this->mDevice->freeMemory(
                        *this->mPrimaryMemory,
                        (vk::Optional<const vk::AllocationCallbacks>)
                nullptr);
            }

            if (this->mFreeStagingMemory) {
                this->mDevice->freeMemory(
                        *this->mStagingMemory,
                        (vk::Optional<const vk::AllocationCallbacks>)
                nullptr);
            }
        }

        TensorTypes tensorType() {
            return this->mTensorType;
        }

        std::shared_ptr <vk::Buffer> primary_buffer() {
            return mPrimaryBuffer;
        }

        void recordCopyFrom(const vk::CommandBuffer &commandBuffer,
                            std::shared_ptr <Tensor> copyFromTensor) {
            vk::DeviceSize bufferSize(this->memorySize());
            vk::BufferCopy copyRegion(0, 0, bufferSize);

            this->recordCopyBuffer(commandBuffer,
                                   copyFromTensor->mPrimaryBuffer,
                                   this->mPrimaryBuffer,
                                   bufferSize,
                                   copyRegion);
        }

        void recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer) {
            vk::DeviceSize bufferSize(this->memorySize());
            vk::BufferCopy copyRegion(0, 0, bufferSize);

            this->recordCopyBuffer(commandBuffer,
                                   this->mStagingBuffer,
                                   this->mPrimaryBuffer,
                                   bufferSize,
                                   copyRegion);
        }

        void recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer) {
            vk::DeviceSize bufferSize(this->memorySize());
            vk::BufferCopy copyRegion(0, 0, bufferSize);

            this->recordCopyBuffer(commandBuffer,
                                   this->mPrimaryBuffer,
                                   this->mStagingBuffer,
                                   bufferSize,
                                   copyRegion);
        }

        void recordPrimaryBufferMemoryBarrier(
                const vk::CommandBuffer &commandBuffer,
                vk::AccessFlagBits srcAccessMask,
                vk::AccessFlagBits dstAccessMask,
                vk::PipelineStageFlagBits srcStageMask,
                vk::PipelineStageFlagBits dstStageMask) {
            this->recordBufferMemoryBarrier(commandBuffer,
                                            *this->mPrimaryBuffer,
                                            srcAccessMask,
                                            dstAccessMask,
                                            srcStageMask,
                                            dstStageMask);
        }

        void recordStagingBufferMemoryBarrier(
                const vk::CommandBuffer &commandBuffer,
                vk::AccessFlagBits srcAccessMask,
                vk::AccessFlagBits dstAccessMask,
                vk::PipelineStageFlagBits srcStageMask,
                vk::PipelineStageFlagBits dstStageMask) {
            this->recordBufferMemoryBarrier(commandBuffer,
                                            *this->mStagingBuffer,
                                            srcAccessMask,
                                            dstAccessMask,
                                            srcStageMask,
                                            dstStageMask);
        }

        vk::DescriptorBufferInfo constructDescriptorBufferInfo() {
            vk::DeviceSize bufferSize = this->memorySize();
            return vk::DescriptorBufferInfo(*this->mPrimaryBuffer,
                                            0, // offset
                                            bufferSize);
        }

        uint32_t size() const {
            return this->mSize;
        }

        uint32_t memorySize() const {
            return this->mSize * this->mDataTypeMemorySize;
        }

        template<typename T>
        T *data() {
            return (T *) this->mRawData;
        }

    private:
        TensorTypes mTensorType = TensorTypes::eDevice;
        uint32_t mSize = 0;
        uint32_t mDataTypeMemorySize = 0;
        void *mRawData = nullptr;
        std::shared_ptr <vk::PhysicalDevice> mPhysicalDevice = nullptr;
        std::shared_ptr <vk::Device> mDevice = nullptr;

        std::shared_ptr <vk::Buffer> mPrimaryBuffer = nullptr;
        bool mFreePrimaryBuffer = false;
        std::shared_ptr <vk::Buffer> mStagingBuffer = nullptr;
        bool mFreeStagingBuffer = false;
        std::shared_ptr <vk::DeviceMemory> mPrimaryMemory = nullptr;
        bool mFreePrimaryMemory = false;
        std::shared_ptr <vk::DeviceMemory> mStagingMemory = nullptr;
        bool mFreeStagingMemory = false;

        void allocateMemoryCreateGPUResources() {
            this->mPrimaryBuffer = std::make_shared<vk::Buffer>();
            this->createBuffer(this->mPrimaryBuffer,
                               this->getPrimaryBufferUsageFlags());
            this->mFreePrimaryBuffer = true;
            this->mPrimaryMemory = std::make_shared<vk::DeviceMemory>();
            this->allocateBindMemory(this->mPrimaryBuffer,
                                     this->mPrimaryMemory,
                                     this->getPrimaryMemoryPropertyFlags());
            this->mFreePrimaryMemory = true;

            if (this->mTensorType == TensorTypes::eDevice) {
                this->mStagingBuffer = std::make_shared<vk::Buffer>();
                this->createBuffer(this->mStagingBuffer,
                                   this->getStagingBufferUsageFlags());
                this->mFreeStagingBuffer = true;
                this->mStagingMemory = std::make_shared<vk::DeviceMemory>();
                this->allocateBindMemory(this->mStagingBuffer,
                                         this->mStagingMemory,
                                         this->getStagingMemoryPropertyFlags());
                this->mFreeStagingMemory = true;
            }
        }

        void createBuffer(std::shared_ptr <vk::Buffer> buffer,
                          vk::BufferUsageFlags bufferUsageFlags) {
            vk::DeviceSize bufferSize = this->memorySize();

            // TODO: Explore having concurrent sharing mode (with option)
            vk::BufferCreateInfo bufferInfo(vk::BufferCreateFlags(),
                                            bufferSize,
                                            bufferUsageFlags,
                                            vk::SharingMode::eExclusive);

            auto result = this->mDevice->createBuffer(&bufferInfo, nullptr, buffer.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
        }

        void allocateBindMemory(std::shared_ptr <vk::Buffer> buffer,
                                std::shared_ptr <vk::DeviceMemory> memory,
                                vk::MemoryPropertyFlags memoryPropertyFlags) {
            vk::PhysicalDeviceMemoryProperties memoryProperties =
                    this->mPhysicalDevice->getMemoryProperties();

            vk::MemoryRequirements memoryRequirements =
                    this->mDevice->getBufferMemoryRequirements(*buffer);

            uint32_t memoryTypeIndex = -1;
            bool memoryTypeIndexFound = false;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
                if (memoryRequirements.memoryTypeBits & (1 << i)) {
                    if (((memoryProperties.memoryTypes[i]).propertyFlags &
                         memoryPropertyFlags) == memoryPropertyFlags) {
                        memoryTypeIndex = i;
                        memoryTypeIndexFound = true;
                        break;
                    }
                }
            }
            if (!memoryTypeIndexFound) {
                throw std::runtime_error(
                        "Memory type index for buffer creation not found");
            }

            vk::MemoryAllocateInfo memoryAllocateInfo(memoryRequirements.size,
                                                      memoryTypeIndex);

            auto result = this->mDevice->allocateMemory(&memoryAllocateInfo, nullptr, memory.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");

            this->mDevice->bindBufferMemory(*buffer, *memory, 0);
        }

        void recordCopyBuffer(const vk::CommandBuffer &commandBuffer,
                              std::shared_ptr <vk::Buffer> bufferFrom,
                              std::shared_ptr <vk::Buffer> bufferTo,
                              vk::DeviceSize bufferSize,
                              vk::BufferCopy copyRegion) {
            commandBuffer.copyBuffer(*bufferFrom, *bufferTo, copyRegion);
        }

        void recordBufferMemoryBarrier(const vk::CommandBuffer &commandBuffer,
                                       const vk::Buffer &buffer,
                                       vk::AccessFlagBits srcAccessMask,
                                       vk::AccessFlagBits dstAccessMask,
                                       vk::PipelineStageFlagBits srcStageMask,
                                       vk::PipelineStageFlagBits dstStageMask) {
            vk::DeviceSize bufferSize = this->memorySize();

            vk::BufferMemoryBarrier bufferMemoryBarrier;
            bufferMemoryBarrier.buffer = buffer;
            bufferMemoryBarrier.size = bufferSize;
            bufferMemoryBarrier.srcAccessMask = srcAccessMask;
            bufferMemoryBarrier.dstAccessMask = dstAccessMask;
            bufferMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            commandBuffer.pipelineBarrier(srcStageMask,
                                          dstStageMask,
                                          vk::DependencyFlags(),
                                          nullptr,
                                          bufferMemoryBarrier,
                                          nullptr);
        }

        // Private util functions
        vk::BufferUsageFlags getPrimaryBufferUsageFlags() {
            switch (this->mTensorType) {
                case TensorTypes::eDevice:
                    return vk::BufferUsageFlagBits::eStorageBuffer |
                           vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst;
                    break;
                case TensorTypes::eHost:
                    return vk::BufferUsageFlagBits::eStorageBuffer |
                           vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst;
                    break;
                case TensorTypes::eStorage:
                    return vk::BufferUsageFlagBits::eStorageBuffer;
                    break;
                default:
                    throw std::runtime_error("Kompute Tensor invalid tensor type");
            }
        }

        vk::MemoryPropertyFlags getPrimaryMemoryPropertyFlags() {
            switch (this->mTensorType) {
                case TensorTypes::eDevice:
                    return vk::MemoryPropertyFlagBits::eDeviceLocal;
                    break;
                case TensorTypes::eHost:
                    return vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent;
                    break;
                case TensorTypes::eStorage:
                    return vk::MemoryPropertyFlagBits::eDeviceLocal;
                    break;
                default:
                    throw std::runtime_error("Kompute Tensor invalid tensor type");
            }
        }

        vk::BufferUsageFlags getStagingBufferUsageFlags() {
            switch (this->mTensorType) {
                case TensorTypes::eDevice:
                    return vk::BufferUsageFlagBits::eTransferSrc |
                           vk::BufferUsageFlagBits::eTransferDst;
                    break;
                default:
                    throw std::runtime_error("Kompute Tensor invalid tensor type");
            }
        }

        vk::MemoryPropertyFlags getStagingMemoryPropertyFlags() {
            switch (this->mTensorType) {
                case TensorTypes::eDevice:
                    return vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent;
                    break;
                default:
                    throw std::runtime_error("Kompute Tensor invalid tensor type");
            }
        }

        void mapRawData() {
            std::shared_ptr <vk::DeviceMemory> hostVisibleMemory = nullptr;

            if (this->mTensorType == TensorTypes::eHost) {
                hostVisibleMemory = this->mPrimaryMemory;
            } else if (this->mTensorType == TensorTypes::eDevice) {
                hostVisibleMemory = this->mStagingMemory;
            } else {
                std::cout << "Tensor mapping data not supported on " << toString(this->tensorType()) << " tensor" << std::endl;
                return;
            }

            vk::DeviceSize bufferSize = this->memorySize();
            // Given we request coherent host memory we don't need to invalidate / flush
            this->mRawData = this->mDevice->mapMemory(
                    *hostVisibleMemory, 0, bufferSize, vk::MemoryMapFlags());
        }

        void unmapRawData() {
            std::shared_ptr <vk::DeviceMemory> hostVisibleMemory = nullptr;

            if (this->mTensorType == TensorTypes::eHost) {
                hostVisibleMemory = this->mPrimaryMemory;
            } else if (this->mTensorType == TensorTypes::eDevice) {
                hostVisibleMemory = this->mStagingMemory;
            } else {
                std::cout << "Tensor mapping data not supported on " << toString(this->tensorType()) << " tensor" << std::endl;
                return;
            }

            vk::DeviceSize bufferSize = this->memorySize();
            vk::MappedMemoryRange mappedRange(*hostVisibleMemory, 0, bufferSize);
            auto result = this->mDevice->flushMappedMemoryRanges(1, &mappedRange);
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mDevice->unmapMemory(*hostVisibleMemory);
        }
    };
}

#endif //MUSECPP_VULKANBUFFER_H
