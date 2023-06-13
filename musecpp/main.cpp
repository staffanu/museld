#include <iostream>
#include <filesystem>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "musevk/VulkanManager.h"
#include "MuseTypes.h"

using namespace std;

void process_file(string filename) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, "MUSE", nullptr, nullptr);

    musevk::VulkanManager manager;
    manager.initVulkan(window);
    vk::Device &device = manager.getDevice();

    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    auto image_available_semaphore = device.createSemaphore(semaphoreInfo);
    auto render_finished_semaphore = device.createSemaphore(semaphoreInfo);
    auto in_flight_fence = device.createFence(fenceInfo);

    {
        auto queue = manager.createCommandQueue(
                std::vector{vk::Semaphore(image_available_semaphore)},
                std::vector{vk::PipelineStageFlags(vk::PipelineStageFlagBits::eBottomOfPipe)});
        Shaders shaders(manager);
        auto decoder = MuseDecoder(filename, shaders, manager, false);
        decoder.Initialize();
        auto image = shaders.getResultImage();

        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        while (decoder.Next()) {
            field_count++;

            if (glfwWindowShouldClose(window))
                break;
            glfwPollEvents();

            //vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
            //vkResetFences(device, 1, &in_flight_fence);

            auto swap_chain_image = manager.acquireNextImage(image_available_semaphore);

            // notice this command queue waits for image_available_semaphore
            queue->enqueueTransitionMemoryLayout(swap_chain_image, vk::Format::eB8G8R8A8Unorm,
                                                 vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

            auto extent = manager.getSwapChainExtent();
            vk::ImageBlit region;
            region.srcOffsets[0] = vk::Offset3D(0, 0, 0);
            region.srcOffsets[1] = vk::Offset3D(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 1);
            region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            region.dstOffsets[1] = vk::Offset3D((int)extent.width, (int)extent.height, 1);
            region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            queue->enqueueBlitImage(image->image(), vk::ImageLayout::eGeneral,
                                    swap_chain_image, vk::ImageLayout::eTransferDstOptimal,
                                    region);

            queue->enqueueTransitionMemoryLayout(swap_chain_image, vk::Format::eB8G8R8A8Unorm,
                                                 vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR);

            queue->evalAsync();
            queue->evalAwait();

            manager.present(swap_chain_image);
        };
        auto t1 = chrono::high_resolution_clock::now();
        long time_us = chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        cout << "Avg " << setprecision(3) << (time_us / 1000.0 / field_count) << " ms/field"
             << " (" << setprecision(3) << 1000000.0 / time_us * field_count << " fields/s)" << endl;
    }

    device.destroy(image_available_semaphore);
    device.destroy(render_finished_semaphore);
    device.destroy(in_flight_fence);

    glfwDestroyWindow(window);

    manager.cleanup();
}

int main(int argc, char *argv[]) {
    try {
        const vector<string> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; ++it) {
            if (!filesystem::exists(*it))
                throw runtime_error("File not found: " + string(*it));
            process_file(*it);
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        cerr << "usage: musecpp [-f] [-s] <input_file> ...\n";
        return EXIT_FAILURE;
    }

    return 0;
}
