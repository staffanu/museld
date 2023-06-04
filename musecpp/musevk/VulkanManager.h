//
// Created by staffanu on 5/24/23.
//

#ifndef MUSECPP_VULKANMANAGER_H
#define MUSECPP_VULKANMANAGER_H

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "VulkanBuffer.h"
#include "CommandQueue.h"

namespace musevk {

    class VulkanManager {
    public:
        VulkanManager();
        VulkanManager(VulkanManager &other) = delete;
        void initWindow(int width, int height);
        void initVulkan();
        bool drawNextFrame(VulkanBuffer &buffer, VulkanManager &manager);
        void cleanup();

        std::shared_ptr<VulkanBuffer> createDeviceBuffer(const std::vector<float> &data);

        std::shared_ptr<VulkanBuffer> createBuffer(
                uint32_t elementTotalCount,
                uint32_t elementMemorySize,
                bool is_host = false,
                bool allow_transfers = false);

        std::shared_ptr<ComputeShader> createComputeShader(
                const std::vector<std::shared_ptr<VulkanBuffer>> &buffers,
                int32_t push_constants_size,
                const std::vector<uint32_t> &spirv,
                const Workgroup &workgroup,
                int max_descriptor_sets = 1);

        std::shared_ptr<CommandQueue> sequence(
                std::vector<vk::Semaphore> wait_semaphores = {},
                std::vector<vk::PipelineStageFlags> wait_dst_stage_masks = {},
                uint32_t totalTimestamps = 0);
    private:
        const std::vector<const char *> validationLayers = {
                "VK_LAYER_KHRONOS_validation"
        };
        const std::vector<const char *> deviceExtensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
#ifdef NDEBUG
        const bool enableValidationLayers = false;
#else
        const bool enableValidationLayers = true;
#endif

        GLFWwindow *m_window;
        vk::Instance m_instance;
        vk::PhysicalDevice m_physical_device;
        vk::Device m_logical_device;
        vk::SurfaceKHR m_surface;
        vk::Queue m_graphics_queue;
        vk::Queue m_present_queue;
        vk::Queue m_compute_queue;
        vk::SwapchainKHR m_swapChain;
        std::vector<vk::Image> swapChainImages;
        vk::Format m_swap_chain_image_format;
        vk::Extent2D swap_chain_extent;
        vk::Semaphore m_image_available_semaphore;
        vk::Semaphore m_render_finished_semaphore;
        vk::Fence m_in_flight_fence;

        void createInstance();
        bool checkValidationLayerSupport();
        void createSurface();
        void pickPhysicalDevice();
        bool isDeviceSuitable(vk::PhysicalDevice &device);
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
    };
}

#endif //MUSECPP_VULKANMANAGER_H
