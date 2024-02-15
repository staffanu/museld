//
// Created by staffanu on 5/24/23.
//

#include <limits>
#include <set>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.hpp>
#include <fmt/format.h>
#include "VulkanManager.h"
#include "VulkanBuffer.h"
#include "HalfFloatUtil.h"

using namespace std;

namespace musevk {
    VulkanManager::VulkanManager(Logger &log)
    : m_log(log) {}

    void VulkanManager::initVulkan(GLFWwindow *window, bool no_sync) {
        m_window = window;
        m_no_sync = no_sync;
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        m_memory_allocator = make_shared<MemoryAllocator>(m_physical_device, m_logical_device);
        createSwapChain();

        auto indices = findQueueFamilies(m_physical_device);
        uint32_t queue_index = indices.graphicsAndComputeFamily.value();

        vk::CommandPoolCreateInfo command_pool_info(vk::CommandPoolCreateFlags(
                vk::CommandPoolCreateFlagBits::eResetCommandBuffer), queue_index);
        m_command_pool = m_logical_device.createCommandPool(command_pool_info);
    }

    void VulkanManager::cleanup() {
        m_logical_device.destroy(m_command_pool);
        m_logical_device.waitIdle();
        cleanupSwapChain();
        m_logical_device.destroy();
        m_instance.destroy(m_surface);
        if (enableValidationLayers) {
            vk::DispatchLoaderDynamic dynamic_loader(m_instance, vkGetInstanceProcAddr);
            m_instance.destroyDebugUtilsMessengerEXT(m_debug_messenger, nullptr, dynamic_loader);
        }
        m_instance.destroy();
    }

    void VulkanManager::cleanupSwapChain() {
        m_logical_device.destroy(m_swap_chain);
    }

    void VulkanManager::recreateSwapChain() {
        vkDeviceWaitIdle(m_logical_device);
        cleanupSwapChain();
        createSwapChain();
    }

    bool recreate_swap_chain_next_call = false;

    vk::Image &VulkanManager::acquireNextImage(vk::Semaphore image_available_semaphore) {
        if (recreate_swap_chain_next_call) {
            recreateSwapChain();
            recreate_swap_chain_next_call = false;
        }

        uint32_t image_index;
        vk::Result result;
        do {
            result = m_logical_device.acquireNextImageKHR(m_swap_chain, UINT64_MAX,
                                                          image_available_semaphore, nullptr,
                                                          &image_index);
            if (result == vk::Result::eSuboptimalKHR) {
                m_log.warn(eVideo, "acquireNextImageKHR: eSuboptimalKHR");
                recreate_swap_chain_next_call = true;
            } else if (result == vk::Result::eErrorOutOfDateKHR) {
                m_log.warn(eVideo, "acquireNextImageKHR: eErrorOutOfDateKHR");
                recreateSwapChain();
            } else if (result != vk::Result::eSuccess)
                throw runtime_error("acquireNextImageKHR failed");
        } while (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR);

        return m_swap_chain_images[image_index];
    }

    void VulkanManager::present(vk::Image image) {
        uint32_t image_index;
        for (image_index = 0; image_index < m_swap_chain_images.size(); image_index++)
            if (m_swap_chain_images[image_index] == image)
                break;
        if (image_index == m_swap_chain_images.size())
            throw runtime_error("present called for image not in swap chain");

        vk::PresentInfoKHR presentInfo{};
        vk::Semaphore signalSemaphores[] = {};//render_finished_semaphore};
        presentInfo.waitSemaphoreCount = 0; // 1 TODO!
        presentInfo.pWaitSemaphores = signalSemaphores;
        vk::SwapchainKHR swapChains[] = {m_swap_chain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &image_index;
        presentInfo.pResults = nullptr; // Optional
        auto result = m_present_queue.presentKHR(presentInfo);
        if (result == vk::Result::eSuboptimalKHR)
            m_log.warn(eVideo, "presentKHR: eSuboptimalKHR");
        else if (result != vk::Result::eSuccess)
            throw runtime_error("presentKHR failed");
    }

    shared_ptr<VulkanBuffer> VulkanManager::createDeviceBuffer(const vector<float> &data) {
        auto host_buffer = VulkanBuffer(*m_memory_allocator, m_logical_device, data.size(), 2, true, true /* unused */);
        for (int i = 0; i < data.size(); i++)
            host_buffer.data<ushort>()[i] = HalfFloatUtil::float_to_half(data[i]);
        auto device_buffer = make_shared<VulkanBuffer>(*m_memory_allocator, m_logical_device, data.size(), 2, false, true);
        auto sq = createCommandBuffer();
        sq->begin();
        sq->enqueueCopyBuffer(host_buffer, *device_buffer);
        sq->submit({}, {}, {});
        sq->wait();
        return device_buffer;
    }

    shared_ptr<VulkanBuffer> VulkanManager::createBuffer(
            uint32_t elementTotalCount,
            uint32_t elementMemorySize,
            bool is_host_visible,
            bool allow_transfers) {
        return make_shared<VulkanBuffer>(*m_memory_allocator, m_logical_device,
                                              elementTotalCount, elementMemorySize,
                                              is_host_visible, allow_transfers);
    }

    shared_ptr<VulkanImage> VulkanManager::createImage(uint32_t width, uint32_t height, std::optional<vk::ImageLayout> initial_layout) {
        auto image = make_shared<VulkanImage>(*m_memory_allocator, m_logical_device, width, height);
        if (initial_layout.has_value()) {
            auto command_buffer = createCommandBuffer();
            command_buffer->begin();
            image->enqueueTransitionLayout(*command_buffer, initial_layout.value(),
                                           vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe,
                                           vk::AccessFlags(), vk::AccessFlags()); // only command in buffer so synchronization doesn't matter
            command_buffer->submit({}, {}, {});
            command_buffer->wait();
        }
        return image;
    }

    shared_ptr<ComputeShader> VulkanManager::createComputeShader(
            string name,
            const vector<shared_ptr<VulkanMemoryObject>> &buffers,
            int32_t push_constants_size,
            const vector<uint32_t> &spirv,
            const Workgroup &workgroup,
            int max_descriptor_sets) {
        return make_shared<ComputeShader>(
                name,
                m_logical_device,
                buffers, push_constants_size, spirv, workgroup, max_descriptor_sets);
    }

    shared_ptr<ComputeShader> VulkanManager::createComputeShader(
            string name,
            const vector<MemoryObjectType> &buffer_types,
            int32_t push_constants_size,
            const vector<uint32_t> &spirv,
            const Workgroup &workgroup,
            int max_descriptor_sets) {
        return make_shared<ComputeShader>(
                name,
                m_logical_device,
                buffer_types, push_constants_size, spirv, workgroup, max_descriptor_sets);
    }

    shared_ptr<CommandBuffer> VulkanManager::createCommandBuffer(
            TimestampQueryPool *timestamp_query_pool) {
        return make_shared<CommandBuffer>(
                m_command_pool,
                m_logical_device,
                m_compute_queue,
                timestamp_query_pool);
    }

    void VulkanManager::createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw runtime_error("validation layers requested, but not available!");
        }

        vk::ApplicationInfo appInfo("Muse Decoder", VK_MAKE_VERSION(1, 0, 0), "No Engine", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_2);
        vk::InstanceCreateInfo createInfo(c_instance_create_flags, &appInfo);

        uint32_t glfwExtensionCount = 0;
        const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        extensions.insert(extensions.cend(), c_instance_extensions.cbegin(), c_instance_extensions.cend());

        auto debugUtilsMessengerCreateInfo = createDebugMessengerCreateInfo();

        if (enableValidationLayers) {
            createInfo.enabledLayerCount = c_validation_layers.size();
            createInfo.ppEnabledLayerNames = c_validation_layers.data();
            extensions.insert(extensions.cend(), c_validation_extensions.cbegin(), c_validation_extensions.cend());
            createInfo.pNext = &debugUtilsMessengerCreateInfo;
        } else {
            createInfo.enabledLayerCount = 0;
        }

        createInfo.enabledExtensionCount = extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

        m_instance = vk::createInstance(createInfo);

        if (enableValidationLayers) {
            vk::DispatchLoaderDynamic dynamic_loader(m_instance, vkGetInstanceProcAddr);
            m_debug_messenger = m_instance.createDebugUtilsMessengerEXT(
                    debugUtilsMessengerCreateInfo, nullptr, dynamic_loader);
        }
    }

    vk::DebugUtilsMessengerCreateInfoEXT VulkanManager::createDebugMessengerCreateInfo() {
        vk::DebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.messageSeverity =
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
        createInfo.messageType =
                vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;;
        createInfo.pfnUserCallback = (PFN_vkDebugUtilsMessengerCallbackEXT)debugCallback;
        createInfo.pUserData = this;
        return createInfo;
    }

    bool VulkanManager::checkValidationLayerSupport() {
        auto available_layers = vk::enumerateInstanceLayerProperties();

        for (const char *layerName: c_validation_layers) {
            bool layerFound = false;
            for (const auto &layerProperties: available_layers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }
            if (!layerFound)
                return false;
        }
        return true;
    }

    void VulkanManager::createSurface() {
        VkSurfaceKHR surface;
        auto result = glfwCreateWindowSurface(m_instance, m_window, nullptr, &surface);
        if (result != VK_SUCCESS) {
            throw runtime_error("failed to create window surface!");
        }
        m_surface = vk::SurfaceKHR(surface);
    }

    void VulkanManager::pickPhysicalDevice() {
        auto devices = m_instance.enumeratePhysicalDevices();
        bool found = false;
        for (auto &device: devices) {
            auto properties = device.getProperties();
            m_log.debug(eVideo, fmt::format("Checking device {}", string(properties.deviceName)));
            if (isDeviceSuitable(device)) {
                found = true;
                m_physical_device = device;
                m_log.info(eVideo, fmt::format("Picked device {}", string(properties.deviceName)));
                break;
            }
        }
        if (!found)
            throw runtime_error("failed to find a suitable GPU!");
    }

    bool VulkanManager::isDeviceSuitable(vk::PhysicalDevice &device) {
        if (!checkDeviceFeaturesSupport(device))
            return false;
        if (!checkDeviceExtensionSupport(device))
            return false;

        QueueFamilyIndices indices = findQueueFamilies(device);
        if (!indices.isComplete())
            return false;

        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        if (swapChainSupport.formats.empty() || swapChainSupport.presentModes.empty())
            return false;

        return true;
    }

    bool VulkanManager::checkDeviceFeaturesSupport(vk::PhysicalDevice &device) {
        auto features2 = device.getFeatures2<
                vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceVulkan11Features,
                vk::PhysicalDeviceVulkan12Features>();

        bool storageBuffer16BitAccess = features2.get<vk::PhysicalDeviceVulkan11Features>().storageBuffer16BitAccess;
        bool uniformAndStorageBuffer16BitAccess = features2.get<vk::PhysicalDeviceVulkan11Features>().uniformAndStorageBuffer16BitAccess;
        bool shaderFloat16 = features2.get<vk::PhysicalDeviceVulkan12Features>().shaderFloat16;

        m_log.debug(eVideo, fmt::format("device features: storageBuffer16BitAccess: {}, uniformAndStorageBuffer16BitAccess: {}, shaderFloat16: {}",
                                        storageBuffer16BitAccess, uniformAndStorageBuffer16BitAccess, shaderFloat16));

        return storageBuffer16BitAccess && uniformAndStorageBuffer16BitAccess && shaderFloat16;
    }

    bool VulkanManager::checkDeviceExtensionSupport(vk::PhysicalDevice &device) {
        auto availableExtensions = device.enumerateDeviceExtensionProperties();

        set<string> requiredExtensions(c_device_extensions.begin(), c_device_extensions.end());

        for (const auto &extension: availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    VulkanManager::QueueFamilyIndices VulkanManager::findQueueFamilies(vk::PhysicalDevice &device) {
        QueueFamilyIndices indices{};
        auto queueFamilies = device.getQueueFamilyProperties();

        int i = 0;
        for (const auto &queueFamily: queueFamilies) {
            if ((queueFamily.queueFlags & vk::QueueFlagBits::eGraphics) &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eCompute)) {
                indices.graphicsAndComputeFamily = i;
            }
            auto presentSupport = device.getSurfaceSupportKHR(i, m_surface);
            if (presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) {
                break;
            }
            i++;
        }

        return indices;
    }

    void VulkanManager::createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(m_physical_device);

        vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
        set<uint32_t> uniqueQueueFamilies = {indices.graphicsAndComputeFamily.value(),
                                                  indices.presentFamily.value()};

        float queuePriority = 1.0f;
        for (uint32_t queueFamily: uniqueQueueFamilies) {
            vk::DeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        auto device_vulkan_12_features = vk::PhysicalDeviceVulkan12Features();
        device_vulkan_12_features.shaderFloat16 = VK_TRUE;

        auto device_vulkan_11_features = vk::PhysicalDeviceVulkan11Features();
        device_vulkan_11_features.storageBuffer16BitAccess = VK_TRUE;
        device_vulkan_11_features.uniformAndStorageBuffer16BitAccess = VK_TRUE;
        device_vulkan_11_features.pNext = &device_vulkan_12_features;

        auto device_features = vk::PhysicalDeviceFeatures2();
        device_features.pNext = &device_vulkan_11_features;

        vk::DeviceCreateInfo createInfo{};
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();

        createInfo.pNext = &device_features;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(c_device_extensions.size());
        createInfo.ppEnabledExtensionNames = c_device_extensions.data();

        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(c_validation_layers.size());
            createInfo.ppEnabledLayerNames = c_validation_layers.data();
        } else {
            createInfo.enabledLayerCount = 0;
        }
        m_logical_device = m_physical_device.createDevice(createInfo);
        m_graphics_queue = m_logical_device.getQueue(indices.graphicsAndComputeFamily.value(), 0);
        m_present_queue = m_logical_device.getQueue(indices.presentFamily.value(), 0);
        m_compute_queue = m_logical_device.getQueue(indices.graphicsAndComputeFamily.value(), 0);
    }

    VulkanManager::SwapChainSupportDetails VulkanManager::querySwapChainSupport(vk::PhysicalDevice &device) {
        auto capabilities = device.getSurfaceCapabilitiesKHR(m_surface);
        auto formats = device.getSurfaceFormatsKHR(m_surface);
        auto present_modes = device.getSurfacePresentModesKHR(m_surface);

        return SwapChainSupportDetails{capabilities, formats, present_modes};
    }

    vk::SurfaceFormatKHR
    VulkanManager::chooseSwapSurfaceFormat(const vector<vk::SurfaceFormatKHR> &availableFormats) {
        for (const auto &availableFormat: availableFormats) {
            if (availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
                availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return availableFormat;
            }
        }

        return availableFormats[1]; // FIXME: UNORM -- otherwise we can't use the VK_IMAGE_USAGE_STORAGE_BIT flag for the swapchain
    }

    vk::PresentModeKHR
    VulkanManager::chooseSwapPresentMode(const vector<vk::PresentModeKHR> &availablePresentModes) {
        for (const auto &availablePresentMode: availablePresentModes) {
            if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
                //return availablePresentMode;
            }
        }
        // FIXME: what to use?
        return m_no_sync ? vk::PresentModeKHR::eImmediate : vk::PresentModeKHR::eFifo;
    }

    vk::Extent2D VulkanManager::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities) {
        if (capabilities.currentExtent.width != numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            int width, height;
            glfwGetFramebufferSize(m_window, &width, &height);

            vk::Extent2D actualExtent = {
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height)
            };

            actualExtent.width = clamp(actualExtent.width, capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
            actualExtent.height = clamp(actualExtent.height, capabilities.minImageExtent.height,
                                             capabilities.maxImageExtent.height);

            return actualExtent;
        }
    }

    void VulkanManager::createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(m_physical_device);

        auto surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        auto presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        auto extent = chooseSwapExtent(swapChainSupport.capabilities);

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }
        vk::SwapchainCreateInfoKHR createInfo{};
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = vk::ImageUsageFlagBits::eTransferDst; // VK_IMAGE_USAGE_STORAGE_BIT; // ; // | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        QueueFamilyIndices indices = findQueueFamilies(m_physical_device);
        uint32_t queueFamilyIndices[] = {indices.graphicsAndComputeFamily.value(), indices.presentFamily.value()};

        if (indices.graphicsAndComputeFamily != indices.presentFamily) {
            createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = vk::SharingMode::eExclusive;
            createInfo.queueFamilyIndexCount = 0; // Optional
            createInfo.pQueueFamilyIndices = nullptr; // Optional
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = nullptr;

        m_swap_chain = m_logical_device.createSwapchainKHR(createInfo);
        m_swap_chain_images = m_logical_device.getSwapchainImagesKHR(m_swap_chain);

        m_swap_chain_image_format = surfaceFormat.format;
        m_swap_chain_extent = extent;
        m_log.info(eVideo, fmt::format("Swap chain created with extent {}x{}", extent.width, extent.height));
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL VulkanManager::debugCallback(
            vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
            vk::DebugUtilsMessageTypeFlagBitsEXT message_type,
            const vk::DebugUtilsMessengerCallbackDataEXT *callback_data,
            void *user_data) {
        return ((VulkanManager *)user_data)->debugCallbackMember(message_severity, message_type, callback_data);
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL VulkanManager::debugCallbackMember(
            vk::DebugUtilsMessageSeverityFlagBitsEXT message_severity,
            vk::DebugUtilsMessageTypeFlagBitsEXT message_type,
            const vk::DebugUtilsMessengerCallbackDataEXT* callback_data) {
        if (message_severity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
            LogPriority severity;
            switch (message_severity) {
                case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
                    severity = LogPriority::eDebug;
                    break;
                case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
                    severity = LogPriority::eInfo;
                    break;
                case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
                    severity = LogPriority::eWarn;
                    break;
                case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
                    severity = LogPriority::eError;
                    break;
                default:
                    throw runtime_error(fmt::format("Unknown message_severity {}", (int)message_severity));
            }
            string type_string;
            switch (message_type) {
                case vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance:
                    type_string = "performance";
                    break;
                case vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation:
                    type_string = "validation";
                    break;
                case vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral:
                    type_string = "general";
                    break;
                case vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding:
                    type_string = "device address binding";
                    break;
                default:
                    type_string = "???";
                    break;
            }

            m_log.log(severity, eVideo, fmt::format("Vulkan validation: ({}) {}", type_string, callback_data->pMessage));
        }

        return VK_FALSE;
    }
}
