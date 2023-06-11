//
// Created by staffanu on 5/24/23.
//

#ifndef MUSECPP_VULKANMANAGER_H
#define MUSECPP_VULKANMANAGER_H

#define GLFW_INCLUDE_VULKAN
#include <string>
#include <GLFW/glfw3.h>
#include "VulkanBuffer.h"
#include "CommandQueue.h"
#include "VulkanImage.h"

namespace musevk {

    class VulkanManager {
    public:
        VulkanManager();
        VulkanManager(VulkanManager &other) = delete;
        void initWindow(int width, int height);
        void initVulkan();
        bool drawNextFrame(VulkanImage &image);
        void cleanup();

        std::shared_ptr<VulkanBuffer> createDeviceBuffer(const std::vector<float> &data);

        std::shared_ptr<VulkanBuffer> createBuffer(
                uint32_t elementTotalCount,
                uint32_t elementMemorySize,
                bool is_host = false,
                bool allow_transfers = false);

        std::shared_ptr<ComputeShader> createComputeShader(
                std::string name,
                const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
                int32_t push_constants_size,
                const std::vector<uint32_t> &spirv,
                const Workgroup &workgroup,
                int max_descriptor_sets = 1);

        std::shared_ptr<ComputeShader> createComputeShader(
                std::string name,
                const std::vector<MemoryObjectType> &buffer_types,
                int32_t push_constants_size,
                const std::vector<uint32_t> &spirv,
                const Workgroup &workgroup,
                int max_descriptor_sets = 1);

        std::shared_ptr<VulkanImage> createImage(uint32_t width, uint32_t height);

        std::shared_ptr<CommandQueue> createCommandQueue(
                std::vector<vk::Semaphore> wait_semaphores = {},
                std::vector<vk::PipelineStageFlags> wait_dst_stage_masks = {},
                uint32_t totalTimestamps = 0);
    private:
        void createInstance();
        bool checkValidationLayerSupport();
        void createSurface();
        void pickPhysicalDevice();
        bool isDeviceSuitable(vk::PhysicalDevice &device);
        bool checkDeviceFeaturesSupport(vk::PhysicalDevice &device);
        bool checkDeviceExtensionSupport(vk::PhysicalDevice &device);

        struct QueueFamilyIndices {
            std::optional<uint32_t> graphicsAndComputeFamily;
            std::optional<uint32_t> presentFamily;

            bool isComplete() const {
                return graphicsAndComputeFamily.has_value() && presentFamily.has_value();
            }
        };
        QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice &device);
        void createLogicalDevice();

        struct SwapChainSupportDetails {
            vk::SurfaceCapabilitiesKHR capabilities;
            std::vector<vk::SurfaceFormatKHR> formats;
            std::vector<vk::PresentModeKHR> presentModes;
        };
        SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice &device);
        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &availableFormats);
        vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR> &availablePresentModes);
        vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities);
        void createSwapChain();
        void createSyncObjects();

        const std::vector<const char *> c_validation_layers = {
                "VK_LAYER_KHRONOS_validation"
        };
        const std::vector<const char *> c_instance_extensions = {
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
#ifdef __APPLE__
                VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
        };
        const std::vector<const char *> c_device_extensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
                VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
                VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
//                VK_AMD_GPU_SHADER_HALF_FLOAT_EXTENSION_NAME,
//                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        };
#ifdef NDEBUG
        const bool enableValidationLayers = false;
#else
        const bool enableValidationLayers = true;
#endif
        GLFWwindow *m_window;
        std::shared_ptr<CommandQueue> m_command_queue;

        vk::Instance m_instance;
        vk::PhysicalDevice m_physical_device;
        vk::Device m_logical_device;
        vk::SurfaceKHR m_surface;
        vk::Queue m_graphics_queue;
        vk::Queue m_present_queue;
        vk::Queue m_compute_queue;
        vk::SwapchainKHR m_swapChain;
        std::vector<vk::Image> m_swap_chain_images;
        vk::Format m_swap_chain_image_format;
        vk::Extent2D swap_chain_extent;
        vk::Semaphore m_image_available_semaphore;
        vk::Semaphore m_render_finished_semaphore;
        vk::Fence m_in_flight_fence;
    };
}

#endif //MUSECPP_VULKANMANAGER_H
