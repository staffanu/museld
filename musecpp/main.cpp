#include <iostream>
#include <filesystem>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "musevk/VulkanManager.h"
#include "MuseTypes.h"
#include "AudioPlayback.h"
#include "InputReader.h"
#include "ResamplingInputReader.h"
#include "BigEndian16MHzInputReader.h"

using namespace std;

void process_file(const string& executable_dir, InputReader &reader,
                  bool decode_all_fields, bool full_screen, bool no_sync) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                          "MUSE", full_screen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);

    musevk::VulkanManager manager;
    manager.initVulkan(window, no_sync);
    vk::Device &device = manager.getDevice();

    {
        std::vector<std::shared_ptr<musevk::VulkanBuffer>> input_vulkan_buffers{};
        for (int i = 0; i < 2; i++)
            input_vulkan_buffers.push_back(
                    manager.createBuffer(MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(uint16_t), true, true));
        if (!reader.initialize(input_vulkan_buffers))
            throw runtime_error("InputReader initialization failed");
    }

    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    auto image_available_semaphore = device.createSemaphore(semaphoreInfo);
    auto render_finished_semaphore = device.createSemaphore(semaphoreInfo);
    auto in_flight_fence = device.createFence(fenceInfo);

    AudioPlayback audio_playback;

    AudioDecoder::AudioMode audio_mode;
    size_t audio_sample_count;
    AudioDecoder::AudioFrame audio_samples[AudioDecoder::c_max_output_samples];

    {
        auto queue = manager.createCommandQueue(
                std::vector{vk::Semaphore(image_available_semaphore)},
                std::vector{vk::PipelineStageFlags(vk::PipelineStageFlagBits::eBottomOfPipe)});
        Shaders shaders(executable_dir, manager);
        auto decoder = MuseDecoder(reader,
                                   shaders,
                                   manager,
                                   true, // decode_video
                                   decode_all_fields,
                                   true, // decode_audio
                                   false); // benchmark_shaders
        if (!decoder.initialize())
            throw runtime_error("MuseDecoder initialization failed");
        auto image = shaders.getResultImage();

        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        bool paused = false;
        // We skip calling next every other field if decode_all_fields is false
        while ((!decode_all_fields && field_count % 2 == 1) || paused ||
                decoder.next(audio_mode, audio_sample_count, audio_samples)) {

            if (!paused)
                field_count++;

            if (glfwWindowShouldClose(window))
                break;
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS && full_screen) {
                glfwSetWindowMonitor(window, nullptr, 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                full_screen = false;
            }
            if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS && !full_screen) {
                glfwSetWindowMonitor(window, glfwGetPrimaryMonitor(), 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                full_screen = true;
            }
            if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
                paused = !paused;
                while (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
                    usleep(100000);
                    glfwPollEvents();
                }
            }

            if (audio_sample_count != 0 && audio_mode != AudioDecoder::MODE_UNKNOWN &&
                field_count % 2 == 1 && !paused)
                audio_playback.add_samples(audio_mode, audio_sample_count, audio_samples);

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
        auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        cout << "Avg " << setprecision(3) << (time_us / 1000.0 / field_count * 2) << " ms/frame"
             << " (" << setprecision(3) << 1000000.0 / time_us * field_count / 2 << " frames/s)" << endl;
    }

    device.destroy(image_available_semaphore);
    device.destroy(render_finished_semaphore);
    device.destroy(in_flight_fence);

    reader.cleanup();
    manager.cleanup();

    audio_playback.cleanup();

    glfwDestroyWindow(window);
}

void usage() {
    cerr << "usage: musecpp [--full-frames-only] [-all-fields] <input_file> ...\n";
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    enum InputFormat {
        eOverSampledBytes,
        eBigEndianShorts,
        eUnknown
    };
    std::string executable(argv[0]);
    std::string executable_dir = executable.substr(0, executable.find_last_of('/'));
    bool decode_all_fields = true;
    bool full_screen = false;
    bool no_sync = false;
    InputFormat input_format = eUnknown;

    try {
        const vector<string> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; it++) {
            if (*it == "--resample")
                input_format = eOverSampledBytes;
            else if (*it == "--big-endian")
                input_format = eBigEndianShorts;
            else if (*it == "--full-frames-only")
                decode_all_fields = false;
            else if (*it == "--all-fields")
                decode_all_fields = true;
            else if (*it == "--full-screen")
                full_screen = true;
            else if (*it == "--no-sync")
                no_sync = true;
            else if (*it == "--help")
                usage();
            else {
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                InputReader *reader;
                switch (input_format) {
                    case eOverSampledBytes:
                        reader = new ResamplingInputReader(*it, 54000000, true);
                        break;
                    case eBigEndianShorts:
                        reader = new BigEndian16MHzInputReader(*it, true);
                        break;
                    case eUnknown:
                    default:
                        throw runtime_error("No input format specified");
                }
                process_file(executable_dir, *reader, decode_all_fields, full_screen, no_sync);
            }
        }
    } catch (const exception &x) {
        cerr << "musecpp: " << x.what() << '\n';
        return EXIT_FAILURE;
    }

    return 0;
}
