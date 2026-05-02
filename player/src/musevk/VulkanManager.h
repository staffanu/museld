//
// Created by staffanu on 5/24/23.
//

#ifndef MUSECPP_VULKANMANAGER_H
#define MUSECPP_VULKANMANAGER_H

#define GLFW_INCLUDE_VULKAN
#include <string>
#include <optional>
#include <GLFW/glfw3.h>
#include "Size.h"
#include "VulkanBuffer.h"
#include "CommandBuffer.h"
#include "VulkanImage.h"
#include "logging/Logger.h"
#include "HalfFloatUtil.h"

namespace musevk {

    class VulkanManager {
    public:
        explicit VulkanManager(Logger &log);
        VulkanManager(VulkanManager &other) = delete;
        void operator=(const musevk::VulkanManager &) = delete;

        void initVulkan(GLFWwindow *window, bool no_sync);
        void cleanup();

        Logger &getLogger() { return m_log; }
        vk::PhysicalDevice &getPhysicalDevice() { return m_physical_device; }
        vk::Device &getDevice() { return m_logical_device; }
        uint32_t getGraphicsAndComputeFamily() { return m_queue_families.graphicsAndComputeFamily.value(); }
        Queue &getGraphicsAndComputeQueue() { return *m_graphics_and_compute_queue; }
        MemoryAllocator &getMemoryAllocator() { return *m_memory_allocator; }
        vk::PhysicalDeviceProperties &getPhysicalDeviceProperties() { return m_physical_device_properties; }

        vk::Image &acquireNextImage(vk::Semaphore image_available_semaphore);
        void present(vk::Image image);
        vk::Extent2D getSwapChainExtent() { return m_swap_chain_extent; }
        void recreateSwapChain();

    private:
        void cleanupSwapChain();
        void createInstance();
        vk::DebugUtilsMessengerCreateInfoEXT createDebugMessengerCreateInfo();
        bool checkValidationLayerSupport();
        void createSurface();
        void pickPhysicalDevice();

        struct QueueFamilyIndices {
            std::optional<uint32_t> graphicsAndComputeFamily;
            std::optional<uint32_t> presentFamily;

            [[nodiscard]] bool isComplete() const {
                return graphicsAndComputeFamily && presentFamily;
            }
        };
        QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice &device);

        std::optional<QueueFamilyIndices> isDeviceSuitable(vk::PhysicalDevice &device);
        bool checkDeviceFeaturesSupport(vk::PhysicalDevice &device);
        bool checkDeviceExtensionSupport(vk::PhysicalDevice &device);

        void createLogicalDevice();

        struct SwapChainSupportDetails {
            vk::SurfaceCapabilitiesKHR capabilities;
            std::vector<vk::SurfaceFormatKHR> formats;
            std::vector<vk::PresentModeKHR> presentModes;
        };
        SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice &device);
        static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &availableFormats);
        [[nodiscard]] vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR> &availablePresentModes) const;
        [[nodiscard]] vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities) const;
        void createSwapChain();

        static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
            vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
            vk::Flags<vk::DebugUtilsMessageTypeFlagBitsEXT> message_type,
            const vk::DebugUtilsMessengerCallbackDataEXT* callback_data,
            void *user_data);

        VKAPI_ATTR VkBool32 VKAPI_CALL debugCallbackMember(
                vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
                vk::Flags<vk::DebugUtilsMessageTypeFlagBitsEXT> message_type,
                const vk::DebugUtilsMessengerCallbackDataEXT *callback_data);

        const std::vector<const char *> c_validation_layers = {
                "VK_LAYER_KHRONOS_validation"
        };
        const std::vector<const char *> c_validation_extensions = {
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        };
        const std::vector<const char *> c_instance_extensions = {
//                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
#ifdef __APPLE__
                VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
        };

#ifdef __APPLE__
        vk::InstanceCreateFlagBits c_instance_create_flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#else
        vk::InstanceCreateFlagBits c_instance_create_flags = {};
#endif

        const std::vector<const char *> c_device_extensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
//                VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME,
//                VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
//                VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
#ifdef __APPLE__
                "VK_KHR_portability_subset", // FIXME: should really only add if the extension enumeration includes it
#endif
        };
#ifdef NDEBUG
        const bool enableValidationLayers = false;
#else
        const bool enableValidationLayers = true;
#endif
        Logger &m_log;
        GLFWwindow *m_window;
        bool m_no_sync;
        std::unique_ptr<MemoryAllocator> m_memory_allocator;

        vk::Instance m_instance;
        vk::DebugUtilsMessengerEXT m_debug_messenger;
        vk::PhysicalDevice m_physical_device;
        vk::PhysicalDeviceProperties m_physical_device_properties;
        QueueFamilyIndices m_queue_families;
        vk::Device m_logical_device;
        vk::SurfaceKHR m_surface;
        Queue *m_graphics_and_compute_queue;
        Queue *m_present_queue;
        vk::SwapchainKHR m_swap_chain;
        std::vector<vk::Image> m_swap_chain_images;
        vk::Format m_swap_chain_image_format;
        vk::Extent2D m_swap_chain_extent;
    };
}

#endif //MUSECPP_VULKANMANAGER_H
