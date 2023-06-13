//
// Created by staffanu on 5/24/23.
//

#include <set>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.hpp>
#include <limits>
#include "VulkanManager.h"
#include "VulkanBuffer.h"
#include "../MuseTypes.h"

namespace musevk {
    void VulkanManager::initVulkan(GLFWwindow *window) {
        m_window = window;
        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapChain();
    }

    void VulkanManager::cleanup() {
        cleanupSwapChain();
        m_logical_device.destroy();
        m_instance.destroy(m_surface);
        m_instance.destroy();
        glfwTerminate();
    }

    void VulkanManager::cleanupSwapChain() {
        m_logical_device.destroy(m_swap_chain);
    }

    void VulkanManager::recreateSwapChain() {
        vkDeviceWaitIdle(m_logical_device);
        cleanupSwapChain();
        createSwapChain();
    }

    vk::Image &VulkanManager::acquireNextImage(vk::Semaphore image_available_semaphore) {
        uint32_t image_index;
        vk::Result result;
        do {
            result = m_logical_device.acquireNextImageKHR(m_swap_chain, UINT64_MAX,
                                                          image_available_semaphore, VK_NULL_HANDLE,
                                                          &image_index);
            if (result == vk::Result::eSuboptimalKHR || result == vk::Result::eErrorOutOfDateKHR) {
                std::cout << "Suboptimal or Out of date" << std::endl;
                recreateSwapChain();
            } else if (result != vk::Result::eSuccess)
                throw std::runtime_error("acquireNextImageKHR failed");
        } while (result != vk::Result::eSuccess);

        return m_swap_chain_images[image_index];
    }

    void VulkanManager::present(vk::Image image) {
        uint32_t image_index;
        for (image_index = 0; image_index < m_swap_chain_images.size(); image_index++)
            if (m_swap_chain_images[image_index] == image)
                break;
        if (image_index == m_swap_chain_images.size())
            throw std::runtime_error("present called for image not in swap chain");

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
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("presentKHR failed");
    }

    std::shared_ptr<VulkanBuffer> VulkanManager::createDeviceBuffer(const std::vector<float> &data) {
        // from "Accuracy and performance of the lattice Boltzmann method with 64-bit, 32-bit, and customized 16-bit number formats"
        auto as_uint = [](const float x) -> uint { return *(uint*)&x; };
        auto float_to_half = [as_uint](const float x) -> ushort { // IEEE-754 16-bit floating-point format (without infinity): 1-5-10, exp-15, +-131008.0, +-6.1035156E-5, +-5.9604645E-8, 3.311 digits
            const uint b = as_uint(x)+0x00001000; // round-to-nearest-even: add last bit after truncated mantissa
            const uint e = (b&0x7F800000)>>23; // exponent
            const uint m = b&0x007FFFFF; // mantissa; in line below: 0x007FF000 = 0x00800000-0x00001000 = decimal indicator flag - initial rounding
            return (b&0x80000000)>>16 | (e>112)*((((e-112)<<10)&0x7C00)|m>>13) | ((e<113)&(e>101))*((((0x007FF000+m)>>(125-e))+1)>>1) | (e>143)*0x7FFF; // sign : normalized : denormalized : saturate
        };

        auto host_buffer = VulkanBuffer(m_physical_device, m_logical_device, data.size(), 2, true, true /* unused */);
        for (int i = 0; i < data.size(); i++)
            host_buffer.data<ushort>()[i] = float_to_half(data[i]);
        auto device_buffer = std::make_shared<VulkanBuffer>(m_physical_device, m_logical_device, data.size(), 2, false, true);
        auto sq = createCommandQueue();
        sq->enqueueCopyBuffer(host_buffer, *device_buffer);
        sq->evalAsync();
        sq->evalAwait();
        return device_buffer;
    }

    std::shared_ptr<VulkanBuffer> VulkanManager::createBuffer(
            uint32_t elementTotalCount,
            uint32_t elementMemorySize,
            bool is_host,
            bool allow_transfers) {
        return std::make_shared<VulkanBuffer>(m_physical_device, m_logical_device,
                elementTotalCount, elementMemorySize, is_host, allow_transfers);
    }

    std::shared_ptr<VulkanImage> VulkanManager::createImage(uint32_t width, uint32_t height) {
        auto image = std::make_shared<VulkanImage>(m_physical_device, m_logical_device, width, height);

        auto sq = createCommandQueue();
        sq->enqueueTransitionMemoryLayout(image->image(), vk::Format::eB8G8R8A8Unorm,
                                          vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);
        sq->evalAsync();
        sq->evalAwait();

        return image;
    }

    std::shared_ptr<ComputeShader> VulkanManager::createComputeShader(
            std::string name,
            const std::vector<std::shared_ptr<VulkanMemoryObject>> &buffers,
            int32_t push_constants_size,
            const std::vector<uint32_t> &spirv,
            const Workgroup &workgroup,
            int max_descriptor_sets) {
        return std::make_shared<ComputeShader>(
                name,
                m_logical_device,
                buffers, push_constants_size, spirv, workgroup, max_descriptor_sets);
    }

    std::shared_ptr<ComputeShader> VulkanManager::createComputeShader(
            std::string name,
            const std::vector<MemoryObjectType> &buffer_types,
            int32_t push_constants_size,
            const std::vector<uint32_t> &spirv,
            const Workgroup &workgroup,
            int max_descriptor_sets) {
        return std::make_shared<ComputeShader>(
                name,
                m_logical_device,
                buffer_types, push_constants_size, spirv, workgroup, max_descriptor_sets);
    }

    std::shared_ptr<CommandQueue> VulkanManager::createCommandQueue(
            std::vector<vk::Semaphore> wait_semaphores,
            std::vector<vk::PipelineStageFlags> wait_dst_stage_masks,
            uint32_t totalTimestamps) {
        auto indices = findQueueFamilies(m_physical_device);
        uint32_t queue_index = indices.graphicsAndComputeFamily.value();
        return std::make_shared<CommandQueue>(
                m_physical_device,
                m_logical_device,
                m_compute_queue,
                queue_index,
                wait_semaphores,
                wait_dst_stage_masks,
                totalTimestamps);
    }

    void VulkanManager::createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("validation layers requested, but not available!");
        }

        vk::ApplicationInfo appInfo("Muse Decoder", VK_MAKE_VERSION(1, 0, 0), "No Engine", VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_2);
#ifdef __APPLE__
        vk::InstanceCreateFlagBits createFlags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#else
        vk::InstanceCreateFlagBits createFlags = {};
#endif
        vk::InstanceCreateInfo createInfo(createFlags, &appInfo);

        uint32_t glfwExtensionCount = 0;
        const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        extensions.insert(extensions.cend(), c_instance_extensions.cbegin(), c_instance_extensions.cend());

        createInfo.enabledExtensionCount = extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = c_validation_layers.size();
            createInfo.ppEnabledLayerNames = c_validation_layers.data();
        } else {
            createInfo.enabledLayerCount = 0;
        }
        m_instance = vk::createInstance(createInfo);
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
        if (glfwCreateWindowSurface(m_instance, m_window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
        m_surface = vk::SurfaceKHR(surface);
    }

    void VulkanManager::pickPhysicalDevice() {
        auto devices = m_instance.enumeratePhysicalDevices();
        bool found = false;
        for (auto &device: devices) {
            auto properties = device.getProperties();
            std::cout << "Checking device " << properties.deviceName << std::endl;
            if (isDeviceSuitable(device)) {
                found = true;
                m_physical_device = device;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("failed to find a suitable GPU!");
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

        return (features2.get<vk::PhysicalDeviceVulkan11Features>().storageBuffer16BitAccess &&
                features2.get<vk::PhysicalDeviceVulkan11Features>().uniformAndStorageBuffer16BitAccess &&
                features2.get<vk::PhysicalDeviceVulkan12Features>().shaderFloat16);
    }

    bool VulkanManager::checkDeviceExtensionSupport(vk::PhysicalDevice &device) {
        auto availableExtensions = device.enumerateDeviceExtensionProperties();

        std::set<std::string> requiredExtensions(c_device_extensions.begin(), c_device_extensions.end());

        for (const auto &extension: availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    VulkanManager::QueueFamilyIndices VulkanManager::findQueueFamilies(vk::PhysicalDevice &device) {
        QueueFamilyIndices indices;
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

        std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsAndComputeFamily.value(),
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
    VulkanManager::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &availableFormats) {
        for (const auto &availableFormat: availableFormats) {
            if (availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
                availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                //return availableFormat;
            }
        }

        return availableFormats[1]; // FIXME: UNORM -- otherwise we can't use the VK_IMAGE_USAGE_STORAGE_BIT flag for the swapchain
    }

    vk::PresentModeKHR
    VulkanManager::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR> &availablePresentModes) {
        for (const auto &availablePresentMode: availablePresentModes) {
            if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
                return availablePresentMode;
            }
        }
        // FIXME: what to use?
        return vk::PresentModeKHR::eFifo;
    }

    vk::Extent2D VulkanManager::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            int width, height;
            glfwGetFramebufferSize(m_window, &width, &height);

            vk::Extent2D actualExtent = {
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height)
            };

            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height,
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
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        m_swap_chain = m_logical_device.createSwapchainKHR(createInfo);
        m_swap_chain_images = m_logical_device.getSwapchainImagesKHR(m_swap_chain);

        m_swap_chain_image_format = surfaceFormat.format;
        m_swap_chain_extent = extent;
    }
}