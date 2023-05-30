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


#define KP_LOG_DEBUG(...)
#define KP_LOG_INFO(...)
#define KP_LOG_WARN(...) std::cout << "WARN " << fmt::format(__VA_ARGS__) << std::endl;

namespace musevk {
    typedef std::array<uint32_t, 3> Workgroup;

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

        Tensor(std::shared_ptr<vk::PhysicalDevice> physicalDevice,
               std::shared_ptr<vk::Device> device,
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

        ~Tensor() {
            if (this->tensorType() != Tensor::TensorTypes::eStorage) {
                this->unmapRawData();
            }

            if (this->mFreePrimaryBuffer) {
                this->mDevice->destroy(
                        *this->mPrimaryBuffer,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreeStagingBuffer) {
                this->mDevice->destroy(
                        *this->mStagingBuffer,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreePrimaryMemory) {
                this->mDevice->freeMemory(
                        *this->mPrimaryMemory,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreeStagingMemory) {
                this->mDevice->freeMemory(
                        *this->mStagingMemory,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }
        }

        TensorTypes tensorType() {
            return this->mTensorType;
        }

        std::shared_ptr<vk::Buffer> primary_buffer() {
            return mPrimaryBuffer;
        }

        void recordCopyFrom(const vk::CommandBuffer &commandBuffer,
                            std::shared_ptr<Tensor> copyFromTensor) {
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
        TensorTypes mTensorType;
        uint32_t mSize;
        uint32_t mDataTypeMemorySize;
        void *mRawData;
        std::shared_ptr<vk::PhysicalDevice> mPhysicalDevice;
        std::shared_ptr<vk::Device> mDevice;

        std::shared_ptr<vk::Buffer> mPrimaryBuffer;
        bool mFreePrimaryBuffer = false;
        std::shared_ptr<vk::Buffer> mStagingBuffer;
        bool mFreeStagingBuffer = false;
        std::shared_ptr<vk::DeviceMemory> mPrimaryMemory;
        bool mFreePrimaryMemory = false;
        std::shared_ptr<vk::DeviceMemory> mStagingMemory;
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

        void createBuffer(std::shared_ptr<vk::Buffer> buffer,
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

        void allocateBindMemory(std::shared_ptr<vk::Buffer> buffer,
                                std::shared_ptr<vk::DeviceMemory> memory,
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
                              std::shared_ptr<vk::Buffer> bufferFrom,
                              std::shared_ptr<vk::Buffer> bufferTo,
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
            std::shared_ptr<vk::DeviceMemory> hostVisibleMemory = nullptr;

            if (this->mTensorType == TensorTypes::eHost) {
                hostVisibleMemory = this->mPrimaryMemory;
            } else if (this->mTensorType == TensorTypes::eDevice) {
                hostVisibleMemory = this->mStagingMemory;
            } else {
                KP_LOG_WARN(
                        "Kompute Tensor mapping data not supported on {} tensor", toString(this->tensorType()));
                return;
            }

            vk::DeviceSize bufferSize = this->memorySize();
            // Given we request coherent host memory we don't need to invalidate / flush
            this->mRawData = this->mDevice->mapMemory(
                    *hostVisibleMemory, 0, bufferSize, vk::MemoryMapFlags());
        }

        void unmapRawData() {
            std::shared_ptr<vk::DeviceMemory> hostVisibleMemory = nullptr;

            if (this->mTensorType == TensorTypes::eHost) {
                hostVisibleMemory = this->mPrimaryMemory;
            } else if (this->mTensorType == TensorTypes::eDevice) {
                hostVisibleMemory = this->mStagingMemory;
            } else {
                KP_LOG_WARN(
                        "Kompute Tensor mapping data not supported on {} tensor", toString(this->tensorType()));
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

    class Algorithm {
    public:
        template<typename S = float, typename P = float>
        Algorithm(std::shared_ptr<vk::Device> device,
                  const std::vector<std::shared_ptr<Tensor>> &tensors,
                  const std::vector<uint32_t> &spirv,
                  const Workgroup &workgroup,
                  const std::vector<P> &pushConstants = {}) {
            mDevice = device;
            mTensors = tensors;
            mSpirv = spirv;

            if (!pushConstants.empty()) {
                uint32_t memorySize = sizeof(decltype(pushConstants.back()));
                uint32_t size = pushConstants.size();
                uint32_t totalSize = size * memorySize;
                mPushConstantsData = malloc(totalSize);
                memcpy(mPushConstantsData, pushConstants.data(), totalSize);
                mPushConstantsDataTypeMemorySize = memorySize;
                mPushConstantsSize = size;
            }
            mWorkgroup = {workgroup[0],
                          workgroup[1] > 0 ? workgroup[1] : 1,
                          workgroup[2] > 0 ? workgroup[2] : 1};
            createParameters();
            createShaderModule();
            createPipeline();
        }

        void recordDispatch(const vk::CommandBuffer &commandBuffer) {
            commandBuffer.dispatch(mWorkgroup[0], mWorkgroup[1], mWorkgroup[2]);
        }

        void recordBindCore(const vk::CommandBuffer &commandBuffer) {
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute,
                                       *this->mPipeline);
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                             *this->mPipelineLayout,
                                             0, // First set
                                             *this->mDescriptorSet,
                                             nullptr // Dispatcher
            );
        }

        void recordBindPush(const vk::CommandBuffer &commandBuffer) {
            if (this->mPushConstantsSize) {
                commandBuffer.pushConstants(*this->mPipelineLayout,
                                            vk::ShaderStageFlagBits::eCompute,
                                            0,
                                            this->mPushConstantsSize *
                                            this->mPushConstantsDataTypeMemorySize,
                                            this->mPushConstantsData);
            }
        }

        template<typename T>
        void setPushConstants(const std::vector<T> &pushConstants) {
            uint32_t memorySize = sizeof(decltype(pushConstants.back()));
            uint32_t size = pushConstants.size();

            uint32_t totalSize = memorySize * size;
            this->mPushConstantsData = malloc(totalSize);
            memcpy(this->mPushConstantsData, pushConstants.data(), totalSize);
            this->mPushConstantsDataTypeMemorySize = memorySize;
            this->mPushConstantsSize = size;
        }

        const std::vector<std::shared_ptr<Tensor>> &getTensors() {
            return mTensors;
        }

        ~Algorithm() {
            if (this->mFreePipeline && this->mPipeline) {
                this->mDevice->destroy(
                        *this->mPipeline,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreePipelineCache && this->mPipelineCache) {
                this->mDevice->destroy(
                        *this->mPipelineCache,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreePipelineLayout && this->mPipelineLayout) {
                this->mDevice->destroy(
                        *this->mPipelineLayout,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mFreeShaderModule && this->mShaderModule) {
                this->mDevice->destroy(
                        *this->mShaderModule,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            // We don't call freeDescriptorSet as the descriptor pool is not created
            // with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT more at
            // (https://www.khronos.org/registry/vulkan/specs/1.0/html/vkspec.html#VUID-vkFreeDescriptorSets-descriptorPool-00312))
            // if (this->mFreeDescriptorSet && this->mDescriptorSet) {
            //    KP_LOG_DEBUG("Kompute Algorithm Freeing Descriptor Set");
            //    if (!this->mDescriptorSet) {
            //        KP_LOG_WARN(
            //          "Kompute Algorithm Error requested to free descriptor set");
            //    }
            //    this->mDevice->freeDescriptorSets(
            //      *this->mDescriptorPool, 1, this->mDescriptorSet.get());
            //    this->mDescriptorSet = nullptr;
            //}

            if (this->mFreeDescriptorSetLayout && this->mDescriptorSetLayout) {
                this->mDevice->destroy(
                        *this->mDescriptorSetLayout,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
                this->mDescriptorSetLayout = nullptr;
            }

            if (this->mFreeDescriptorPool && this->mDescriptorPool) {
                this->mDevice->destroy(
                        *this->mDescriptorPool,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
                this->mDescriptorPool = nullptr;
            }
        }

    private:
        // -------------- NEVER OWNED RESOURCES
        std::shared_ptr<vk::Device> mDevice;
        std::vector<std::shared_ptr<Tensor>> mTensors;

        // -------------- OPTIONALLY OWNED RESOURCES
        std::shared_ptr<vk::DescriptorSetLayout> mDescriptorSetLayout;
        bool mFreeDescriptorSetLayout = false;
        std::shared_ptr<vk::DescriptorPool> mDescriptorPool;
        bool mFreeDescriptorPool = false;
        std::shared_ptr<vk::DescriptorSet> mDescriptorSet;
        bool mFreeDescriptorSet = false;
        std::shared_ptr<vk::ShaderModule> mShaderModule;
        bool mFreeShaderModule = false;
        std::shared_ptr<vk::PipelineLayout> mPipelineLayout;
        bool mFreePipelineLayout = false;
        std::shared_ptr<vk::PipelineCache> mPipelineCache;
        bool mFreePipelineCache = false;
        std::shared_ptr<vk::Pipeline> mPipeline;
        bool mFreePipeline = false;

        // -------------- ALWAYS OWNED RESOURCES
        std::vector<uint32_t> mSpirv;
        void *mPushConstantsData = nullptr;
        uint32_t mPushConstantsDataTypeMemorySize = 0;
        uint32_t mPushConstantsSize = 0;
        Workgroup mWorkgroup;

        void createShaderModule() {
            vk::ShaderModuleCreateInfo shaderModuleInfo(vk::ShaderModuleCreateFlags(),
                                                        sizeof(uint32_t) *
                                                        this->mSpirv.size(),
                                                        this->mSpirv.data());

            KP_LOG_DEBUG("Kompute Algorithm Creating shader module. ShaderFileSize: {}",
                         this->mSpirv.size());
            this->mFreeShaderModule = true;
            this->mShaderModule = std::make_shared<vk::ShaderModule>();
            auto result = this->mDevice->createShaderModule(&shaderModuleInfo, nullptr, this->mShaderModule.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreeShaderModule = true;
        }

        void createPipeline() {
            vk::PipelineLayoutCreateInfo pipelineLayoutInfo(
                    vk::PipelineLayoutCreateFlags(),
                    1, // Set layout count
                    this->mDescriptorSetLayout.get());

            vk::PushConstantRange pushConstantRange;
            if (this->mPushConstantsSize) {
                pushConstantRange.setStageFlags(vk::ShaderStageFlagBits::eCompute);
                pushConstantRange.setOffset(0);
                pushConstantRange.setSize(this->mPushConstantsDataTypeMemorySize *
                                          this->mPushConstantsSize);

                pipelineLayoutInfo.setPushConstantRangeCount(1);
                pipelineLayoutInfo.setPPushConstantRanges(&pushConstantRange);
            }

            this->mPipelineLayout = std::make_shared<vk::PipelineLayout>();
            auto result = this->mDevice->createPipelineLayout(
                    &pipelineLayoutInfo, nullptr, this->mPipelineLayout.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreePipelineLayout = true;

            std::vector<vk::SpecializationMapEntry> specializationEntries;

            vk::PipelineShaderStageCreateInfo shaderStage(
                    vk::PipelineShaderStageCreateFlags(),
                    vk::ShaderStageFlagBits::eCompute,
                    *this->mShaderModule,
                    "main",
                    nullptr);

            vk::ComputePipelineCreateInfo pipelineInfo(vk::PipelineCreateFlags(),
                                                       shaderStage,
                                                       *this->mPipelineLayout,
                                                       vk::Pipeline(),
                                                       0);

            vk::PipelineCacheCreateInfo pipelineCacheInfo =
                    vk::PipelineCacheCreateInfo();
            this->mPipelineCache = std::make_shared<vk::PipelineCache>();
            result = this->mDevice->createPipelineCache(
                    &pipelineCacheInfo, nullptr, this->mPipelineCache.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreePipelineCache = true;

            vk::Pipeline pipeline =
                    this->mDevice->createComputePipeline(*this->mPipelineCache, pipelineInfo)
                            .value;
            this->mPipeline = std::make_shared<vk::Pipeline>(pipeline);
            this->mFreePipeline = true;
        }

        void createParameters() {
            std::vector<vk::DescriptorPoolSize> descriptorPoolSizes = {
                    vk::DescriptorPoolSize(
                            vk::DescriptorType::eStorageBuffer,
                            static_cast<uint32_t>(this->mTensors.size()) // Descriptor count
                    )
            };

            vk::DescriptorPoolCreateInfo descriptorPoolInfo(
                    vk::DescriptorPoolCreateFlags(),
                    1, // Max sets
                    static_cast<uint32_t>(descriptorPoolSizes.size()),
                    descriptorPoolSizes.data());

            this->mDescriptorPool = std::make_shared<vk::DescriptorPool>();
            auto result = this->mDevice->createDescriptorPool(
                    &descriptorPoolInfo, nullptr, this->mDescriptorPool.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreeDescriptorPool = true;

            std::vector<vk::DescriptorSetLayoutBinding> descriptorSetBindings;
            for (size_t i = 0; i < this->mTensors.size(); i++) {
                descriptorSetBindings.emplace_back(
                        vk::DescriptorSetLayoutBinding(i, // Binding index
                                                       vk::DescriptorType::eStorageBuffer,
                                                       1, // Descriptor count
                                                       vk::ShaderStageFlagBits::eCompute));
            }

            vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutInfo(
                    vk::DescriptorSetLayoutCreateFlags(),
                    static_cast<uint32_t>(descriptorSetBindings.size()),
                    descriptorSetBindings.data());

            this->mDescriptorSetLayout = std::make_shared<vk::DescriptorSetLayout>();
            result = this->mDevice->createDescriptorSetLayout(
                    &descriptorSetLayoutInfo, nullptr, this->mDescriptorSetLayout.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreeDescriptorSetLayout = true;

            vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo(
                    *this->mDescriptorPool,
                    1, // Descriptor set layout count
                    this->mDescriptorSetLayout.get());

            this->mDescriptorSet = std::make_shared<vk::DescriptorSet>();
            result = this->mDevice->allocateDescriptorSets(&descriptorSetAllocateInfo,
                                                           this->mDescriptorSet.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
            this->mFreeDescriptorSet = true;

            for (size_t i = 0; i < this->mTensors.size(); i++) {
                std::vector<vk::WriteDescriptorSet> computeWriteDescriptorSets;

                vk::DescriptorBufferInfo descriptorBufferInfo =
                        this->mTensors[i]->constructDescriptorBufferInfo();

                computeWriteDescriptorSets.emplace_back(
                        vk::WriteDescriptorSet(*this->mDescriptorSet,
                                               i, // Destination binding
                                               0, // Destination array element
                                               1, // Descriptor count
                                               vk::DescriptorType::eStorageBuffer,
                                               nullptr, // Descriptor image info
                                               &descriptorBufferInfo));

                this->mDevice->updateDescriptorSets(computeWriteDescriptorSets,
                                                    nullptr);
            }
        }
    };

    class OpBase {
    public:
        virtual void record(const vk::CommandBuffer &commandBuffer) = 0;
    };

    class OpAlgoDispatch : public OpBase {
    public:
        template<typename T = float>
        OpAlgoDispatch(const std::shared_ptr<Algorithm> &algorithm,
                       const std::vector<T> &pushConstants = {}) {
            this->mAlgorithm = algorithm;
            for (auto &pc: pushConstants) {
                auto *p = reinterpret_cast<const float *>(&pc);
                m_push_constants.emplace_back(*p);
            }
        }

        virtual void record(const vk::CommandBuffer &commandBuffer) override {
            for (const std::shared_ptr<Tensor> &tensor:
                    this->mAlgorithm->getTensors()) {
                tensor->recordPrimaryBufferMemoryBarrier(
                        commandBuffer,
                        vk::AccessFlagBits::eTransferWrite,
                        vk::AccessFlagBits::eShaderRead,
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eComputeShader);
            }

            if (!this->m_push_constants.empty()) {
                this->mAlgorithm->setPushConstants(m_push_constants);
            }

            this->mAlgorithm->recordBindCore(commandBuffer);
            this->mAlgorithm->recordBindPush(commandBuffer);
            this->mAlgorithm->recordDispatch(commandBuffer);
        }

    private:
        // -------------- ALWAYS OWNED RESOURCES
        std::shared_ptr<Algorithm> mAlgorithm;
        std::vector<float> m_push_constants;
        uint32_t mPushConstantsDataTypeMemorySize = 0;
        uint32_t mPushConstantsSize = 0;
    };

    class OpTensorSyncDevice : public OpBase {
    public:
        OpTensorSyncDevice(const std::vector<std::shared_ptr<Tensor>> &tensors) {
            this->mTensors = tensors;
        }

        void record(const vk::CommandBuffer &commandBuffer) override {
            for (size_t i = 0; i < this->mTensors.size(); i++) {
                if (this->mTensors[i]->tensorType() == Tensor::TensorTypes::eDevice) {
                    this->mTensors[i]->recordCopyFromStagingToDevice(commandBuffer);
                }
            }
        }

    private:
        std::vector<std::shared_ptr<Tensor>> mTensors;
    };

    class OpTensorSyncLocal : public OpBase {
    public:
        OpTensorSyncLocal(const std::vector<std::shared_ptr<Tensor>> &tensors) {
            this->mTensors = tensors;
        }

        void record(const vk::CommandBuffer &commandBuffer) override {
            for (size_t i = 0; i < this->mTensors.size(); i++) {
                if (this->mTensors[i]->tensorType() == Tensor::TensorTypes::eDevice) {

                    this->mTensors[i]->recordPrimaryBufferMemoryBarrier(
                            commandBuffer,
                            vk::AccessFlagBits::eShaderWrite,
                            vk::AccessFlagBits::eTransferRead,
                            vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eTransfer);

                    this->mTensors[i]->recordCopyFromDeviceToStaging(commandBuffer);

                    this->mTensors[i]->recordPrimaryBufferMemoryBarrier(
                            commandBuffer,
                            vk::AccessFlagBits::eTransferWrite,
                            vk::AccessFlagBits::eHostRead,
                            vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eHost);
                }
            }
        }

    private:
        std::vector<std::shared_ptr<Tensor>> mTensors;
    };

    class Sequence : public std::enable_shared_from_this<Sequence> {
    public:
        Sequence(std::shared_ptr<vk::PhysicalDevice> physicalDevice,
                 std::shared_ptr<vk::Device> device,
                 std::shared_ptr<vk::Queue> computeQueue,
                 uint32_t queueIndex,
                 std::vector<vk::Semaphore> wait_semaphores,
                 std::vector<vk::PipelineStageFlags> wait_dst_stage_masks,
                 uint32_t totalTimestamps = 0) {
            this->mPhysicalDevice = physicalDevice;
            this->mDevice = device;
            this->mComputeQueue = computeQueue;
            this->mQueueIndex = queueIndex;
            m_wait_semaphores = wait_semaphores;
            m_wait_dst_stage_masks = wait_dst_stage_masks;

            this->createCommandPool();
            this->createCommandBuffer();
            if (totalTimestamps > 0)
                this->createTimestampQueryPool(totalTimestamps +
                                               1); //+1 for the first one
        }

        void recordTransionMemoryLayout(vk::Image image, vk::Format format,
                                        vk::ImageLayout oldLayout, vk::ImageLayout newLayout) {
            vk::ImageMemoryBarrier barrier;
            barrier.oldLayout =  oldLayout;
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
            mCommandBuffer->pipelineBarrier(vk::PipelineStageFlags(vk::PipelineStageFlagBits::eTopOfPipe),
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
            mCommandBuffer->copyBufferToImage(*bufferFrom, imageTo, layout, region);
        }

        std::shared_ptr<Sequence> record(std::shared_ptr<OpBase> op) {
            this->begin();
            op->record(*this->mCommandBuffer);

            this->mOperations.push_back(op);

            if (this->timestampQueryPool)
                this->mCommandBuffer->writeTimestamp(
                        vk::PipelineStageFlagBits::eAllCommands,
                        *this->timestampQueryPool,
                        this->mOperations.size());

            return shared_from_this();
        }

        template<typename T, typename... TArgs>
        std::shared_ptr<Sequence> record(
                std::vector<std::shared_ptr<Tensor>> tensors,
                TArgs &&... params) {
            std::shared_ptr<T> op{new T(tensors, std::forward<TArgs>(params)...)};
            return this->record(op);
        }

        template<typename T, typename... TArgs>
        std::shared_ptr<Sequence> record(std::shared_ptr<Algorithm> algorithm,
                                         TArgs &&... params) {
            std::shared_ptr<T> op{new T(algorithm,
                                        std::forward<TArgs>(params)...)};
            return this->record(op);
        }

        std::shared_ptr<Sequence> eval() {
            return this->evalAsync()->evalAwait();
        }

        template<typename T>
        std::shared_ptr<Sequence> eval(std::vector<std::shared_ptr<Tensor>> tensors) {
            std::shared_ptr<T> op{new T(tensors)};
            return this->eval(op);
        }

        template<typename T, typename... TArgs>
        std::shared_ptr<Sequence> eval(std::shared_ptr<Algorithm> algorithm,
                                       TArgs &&... params) {
            std::shared_ptr<T> op{new T(algorithm,
                                        std::forward<TArgs>(params)...)};
            return this->eval(op);
        }

        std::shared_ptr<Sequence> evalAsync() {
            if (this->isRecording()) {
                this->end();
            }

            if (this->mIsRunning) {
                throw std::runtime_error(
                        "Kompute Sequence evalAsync called when an eval async was "
                        "called without successful wait");
            }

            this->mIsRunning = true;

            vk::SubmitInfo submitInfo(m_wait_semaphores, m_wait_dst_stage_masks, *this->mCommandBuffer);

            this->mFence = this->mDevice->createFence(vk::FenceCreateInfo());

            auto result = this->mComputeQueue->submit(1, &submitInfo, this->mFence);
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");

            return shared_from_this();
        }

        template<typename T, typename... TArgs>
        std::shared_ptr<Sequence> evalAsync(
                std::vector<std::shared_ptr<Tensor>> tensors,
                TArgs &&... params) {
            std::shared_ptr<T> op{new T(tensors, std::forward<TArgs>(params)...)};
            return this->evalAsync(op);
        }

        template<typename T, typename... TArgs>
        std::shared_ptr<Sequence> evalAsync(std::shared_ptr<Algorithm> algorithm,
                                            TArgs &&... params) {
            std::shared_ptr<T> op{new T(algorithm,
                                        std::forward<TArgs>(params)...)};
            return this->evalAsync(op);
        }

        std::shared_ptr<Sequence> evalAwait(uint64_t waitFor = UINT64_MAX) {
            if (!this->mIsRunning) {
                KP_LOG_WARN("Kompute Sequence evalAwait called without existing eval");
                return shared_from_this();
            }

            vk::Result result =
                    this->mDevice->waitForFences(1, &this->mFence, VK_TRUE, waitFor);
            this->mDevice->destroy(
                    this->mFence, (vk::Optional<const vk::AllocationCallbacks>) nullptr);

            this->mIsRunning = false;

            if (result == vk::Result::eTimeout) {
                KP_LOG_WARN("Kompute Sequence evalAwait reached timeout of {}",
                            waitFor);
                return shared_from_this();
            }

            return shared_from_this();
        }

        void clear() {
            if (this->isRecording()) {
                this->end();
            }
        }

        std::vector<std::uint64_t> getTimestamps() {
            if (!this->timestampQueryPool)
                throw std::runtime_error("Timestamp latching not enabled");

            const auto n = this->mOperations.size() + 1;
            std::vector<std::uint64_t> timestamps(n, 0);
            auto result = this->mDevice->getQueryPoolResults(
                    *this->timestampQueryPool,
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
            this->mCommandBuffer->begin(vk::CommandBufferBeginInfo());
            this->mRecording = true;

            // latch the first timestamp before any commands are submitted
            if (this->timestampQueryPool)
                this->mCommandBuffer->writeTimestamp(
                        vk::PipelineStageFlagBits::eAllCommands,
                        *this->timestampQueryPool,
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
                this->mCommandBuffer->end();
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
            if (this->mFreeCommandBuffer) {
                this->mDevice->freeCommandBuffers(
                        *this->mCommandPool, 1, this->mCommandBuffer.get());
            }

            if (this->mFreeCommandPool) {
                this->mDevice->destroy(
                        *this->mCommandPool,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }

            if (this->mOperations.size()) {
                this->mOperations.clear();
            }

            if (this->timestampQueryPool) {
                this->mDevice->destroy(
                        *this->timestampQueryPool,
                        (vk::Optional<const vk::AllocationCallbacks>) nullptr);
            }
        }

    private:
        std::shared_ptr<vk::PhysicalDevice> mPhysicalDevice = nullptr;
        std::shared_ptr<vk::Device> mDevice = nullptr;
        std::shared_ptr<vk::Queue> mComputeQueue = nullptr;
        uint32_t mQueueIndex = -1;

        std::shared_ptr<vk::CommandPool> mCommandPool = nullptr;
        bool mFreeCommandPool = false;
        std::shared_ptr<vk::CommandBuffer> mCommandBuffer = nullptr;
        bool mFreeCommandBuffer = false;

        vk::Fence mFence;
        std::vector<std::shared_ptr<OpBase>> mOperations{};
        std::shared_ptr<vk::QueryPool> timestampQueryPool = nullptr;

        std::vector<vk::Semaphore> m_wait_semaphores;
        std::vector<vk::PipelineStageFlags> m_wait_dst_stage_masks;

        bool mRecording = false;
        bool mIsRunning = false;

        void createCommandPool() {
            this->mFreeCommandPool = true;

            vk::CommandPoolCreateInfo commandPoolInfo(vk::CommandPoolCreateFlags(),
                                                      this->mQueueIndex);
            this->mCommandPool = std::make_shared<vk::CommandPool>();
            auto result = this->mDevice->createCommandPool(
                    &commandPoolInfo, nullptr, this->mCommandPool.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
        }

        void createCommandBuffer() {
            this->mFreeCommandBuffer = true;

            vk::CommandBufferAllocateInfo commandBufferAllocateInfo(
                    *this->mCommandPool, vk::CommandBufferLevel::ePrimary, 1);

            this->mCommandBuffer = std::make_shared<vk::CommandBuffer>();
            auto result = this->mDevice->allocateCommandBuffers(&commandBufferAllocateInfo,
                                                                this->mCommandBuffer.get());
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("error");
        }

        void createTimestampQueryPool(uint32_t totalTimestamps) {
            vk::PhysicalDeviceProperties physicalDeviceProperties =
                    this->mPhysicalDevice->getProperties();

            if (physicalDeviceProperties.limits.timestampComputeAndGraphics) {
                vk::QueryPoolCreateInfo queryPoolInfo;
                queryPoolInfo.setQueryCount(totalTimestamps);
                queryPoolInfo.setQueryType(vk::QueryType::eTimestamp);
                this->timestampQueryPool = std::make_shared<vk::QueryPool>(
                        this->mDevice->createQueryPool(queryPoolInfo));
            } else {
                throw std::runtime_error("Device does not support timestamps");
            }
        }
    };

    class VulkanResources {
    public:
        VulkanResources(VkPhysicalDevice &physical_device, VkDevice &device,
                        VkQueue &computeQueue, uint32_t compute_queue_index)
                : m_physical_device(physical_device), m_device(device),
                  m_compute_queue(computeQueue), m_compute_queue_index(compute_queue_index) {
        }

        //VulkanResources(VulkanResources &other) = delete;
        void operator=(const VulkanResources &) = delete;

        std::shared_ptr<Tensor> tensor(
                const std::vector<float> &data,
                Tensor::TensorTypes tensorType = Tensor::TensorTypes::eDevice) {
            auto tensor = std::make_shared<Tensor>(
                    std::make_shared<vk::PhysicalDevice>(vk::PhysicalDevice(m_physical_device)),
                    std::make_shared<vk::Device>(vk::Device(m_device)),
                    (void *)data.data(), data.size(), sizeof(float), tensorType);
            return tensor;
        }

        std::shared_ptr<Tensor> tensor(
                void *data,
                uint32_t elementTotalCount,
                uint32_t elementMemorySize,
                Tensor::TensorTypes tensorType = Tensor::TensorTypes::eDevice) {
            std::shared_ptr<Tensor> tensor{
                    new Tensor(std::make_shared<vk::PhysicalDevice>(vk::PhysicalDevice(m_physical_device)),
                               std::make_shared<vk::Device>(vk::Device(m_device)),
                               data,
                               elementTotalCount,
                               elementMemorySize,
                               tensorType)};

            return tensor;
        }

        std::shared_ptr<Sequence>
        sequence(std::vector<vk::Semaphore> wait_semaphores = {},
                 std::vector<vk::PipelineStageFlags> wait_dst_stage_masks = {},
                 uint32_t totalTimestamps = 0) {
            std::shared_ptr<Sequence> sq{new Sequence(
                    std::make_shared<vk::PhysicalDevice>(vk::PhysicalDevice(m_physical_device)),
                    std::make_shared<vk::Device>(vk::Device(m_device)),
                    std::make_shared<vk::Queue>(vk::Queue(m_compute_queue)),
                    this->m_compute_queue_index,
                    wait_semaphores,
                    wait_dst_stage_masks,
                    totalTimestamps)};

            return sq;
        }

        template<typename S = float, typename P = float>
        std::shared_ptr<Algorithm> algorithm(
                const std::vector<std::shared_ptr<Tensor>> &tensors,
                const std::vector<uint32_t> &spirv,
                const Workgroup &workgroup,
                const std::vector<P> &pushConstants) {
            std::shared_ptr<Algorithm> algorithm{new Algorithm(
                    std::make_shared<vk::Device>(vk::Device(m_device)),
                    tensors,
                    spirv,
                    workgroup,
                    pushConstants)};

            return algorithm;
        }

    private:
        VkPhysicalDevice m_physical_device;
        VkDevice m_device;
        VkQueue m_compute_queue;
        uint32_t m_compute_queue_index;

    };
}

#endif //MUSECPP_VULKANRESOURCES_H
