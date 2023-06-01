//
// Created by staffanu on 5/24/23.
//

#include <set>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.hpp>
#include "VulkanManager.h"
#include "MuseDecoder.h"
#include "MuseTypes.h"

VulkanManager::VulkanManager() = default;

void VulkanManager::InitWindow() {
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    m_window = glfwCreateWindow(WIDTH, HEIGHT, "MUSE", nullptr, nullptr);
}

void VulkanManager::InitVulkan() {
    createInstance();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapChain();
    createSyncObjects();
}

bool VulkanManager::DrawNextFrame(musevk::Tensor &tensor) {
    if (glfwWindowShouldClose(m_window))
        return false;
    glfwPollEvents();

    //vkWaitForFences(m_logical_device, 1, &m_in_flight_fence, VK_TRUE, UINT64_MAX);
    //vkResetFences(m_logical_device, 1, &m_in_flight_fence);
    uint32_t imageIndex;
    vkAcquireNextImageKHR(m_logical_device, m_swapChain, UINT64_MAX, m_image_available_semaphore, VK_NULL_HANDLE, &imageIndex);

    auto sq = resources().sequence(std::vector{vk::Semaphore(m_image_available_semaphore)},
                                   std::vector{vk::PipelineStageFlags(vk::PipelineStageFlagBits::eBottomOfPipe)});
    sq->begin();

    sq->recordTransionMemoryLayout(swapChainImages[imageIndex], vk::Format::eB8G8R8A8Unorm,
                                   vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

    vk::BufferImageCopy region({}, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                               vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                               {}, {MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 1});
    sq->recordCopyBufferToImage(tensor.primary_buffer(), vk::Image(swapChainImages[imageIndex]),
                                vk::ImageLayout::eTransferDstOptimal, region);

    sq->recordTransionMemoryLayout(swapChainImages[imageIndex], vk::Format::eB8G8R8A8Unorm,
                                   vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR);

    sq->evalAsync();
    sq->evalAwait();
    vk::PresentInfoKHR presentInfo{};

    vk::Semaphore signalSemaphores[] = {m_render_finished_semaphore};
    presentInfo.waitSemaphoreCount = 0; // 1 TODO!
    presentInfo.pWaitSemaphores = signalSemaphores;
    vk::SwapchainKHR swapChains[] = {m_swapChain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.pResults = nullptr; // Optional
    m_present_queue.presentKHR(presentInfo);

    return true;
}

void VulkanManager::Cleanup() {
    m_logical_device.destroy(m_image_available_semaphore);
    m_logical_device.destroy(m_render_finished_semaphore);
    m_logical_device.destroy(m_in_flight_fence);
    m_logical_device.destroy(m_swapChain);
    m_logical_device.destroy();
    m_instance.destroy(m_surface);
    m_instance.destroy();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

musevk::VulkanResources VulkanManager::resources() {
    auto indices = findQueueFamilies(m_physical_device);
    return musevk::VulkanResources(m_physical_device, m_logical_device, m_compute_queue, indices.graphicsAndComputeFamily.value());
}

void VulkanManager::createInstance() {
    if (enableValidationLayers && !checkValidationLayerSupport()) {
        throw std::runtime_error("validation layers requested, but not available!");
    }

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "Muse Decoder";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    vk::InstanceCreateInfo createInfo{};
    createInfo.pApplicationInfo = &appInfo;

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;
    if (enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
    } else {
        createInfo.enabledLayerCount = 0;
    }
    m_instance = vk::createInstance(createInfo);
}

bool VulkanManager::checkValidationLayerSupport() {
    auto available_layers = vk::enumerateInstanceLayerProperties();

    for (const char* layerName : validationLayers) {
        bool layerFound = false;
        for (const auto& layerProperties : available_layers) {
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
    for (auto &device : devices) {
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
    QueueFamilyIndices indices = findQueueFamilies(device);
    bool extensionsSupported = checkDeviceExtensionSupport(device);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    return indices.isComplete() && extensionsSupported && swapChainAdequate;
}

bool VulkanManager::checkDeviceExtensionSupport(vk::PhysicalDevice &device) {
    auto availableExtensions = device.enumerateDeviceExtensionProperties();

    std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

VulkanManager::QueueFamilyIndices VulkanManager::findQueueFamilies(vk::PhysicalDevice &device) {
    QueueFamilyIndices indices;
    auto queueFamilies = device.getQueueFamilyProperties();

    int i = 0;
    for (const auto& queueFamily : queueFamilies) {
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
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsAndComputeFamily.value(), indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        vk::DeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    vk::PhysicalDeviceFeatures deviceFeatures{};
    vk::DeviceCreateInfo createInfo{};
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (enableValidationLayers) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();
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

    return SwapChainSupportDetails { capabilities, formats, present_modes };
}

vk::SurfaceFormatKHR VulkanManager::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == vk::Format::eB8G8R8A8Srgb && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            //return availableFormat;
        }
    }

    return availableFormats[1]; // FIXME: UNORM -- otherwise we can't use the VK_IMAGE_USAGE_STORAGE_BIT flag for the swapchain
}

vk::PresentModeKHR VulkanManager::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
            return availablePresentMode;
        }
    }

    return vk::PresentModeKHR::eFifo;
}

vk::Extent2D VulkanManager::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        int width, height;
        glfwGetFramebufferSize(m_window, &width, &height);

        vk::Extent2D actualExtent = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)
        };

        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return actualExtent;
    }
}

void VulkanManager::createSwapChain() {
    SwapChainSupportDetails swapChainSupport = querySwapChainSupport(m_physical_device);

    auto surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
    auto presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
    auto extent = chooseSwapExtent(swapChainSupport.capabilities);

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
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

    m_swapChain = m_logical_device.createSwapchainKHR(createInfo);
    swapChainImages = m_logical_device.getSwapchainImagesKHR(m_swapChain);

    m_swap_chain_image_format = surfaceFormat.format;
    swap_chain_extent = extent;
}

void VulkanManager::createSyncObjects() {
    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    m_image_available_semaphore = m_logical_device.createSemaphore(semaphoreInfo);
    m_render_finished_semaphore = m_logical_device.createSemaphore(semaphoreInfo);
    m_in_flight_fence = m_logical_device.createFence(fenceInfo);
}
