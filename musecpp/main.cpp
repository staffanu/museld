#include <filesystem>
#include <unistd.h>
#include <fmt/format.h>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "musevk/VulkanManager.h"
#include "MuseTypes.h"
#include "AudioPlayback.h"
#include "InputReader.h"
#include "ResamplingInputReader.h"
#include "PhaseCorrect16MHzInputReader.h"
#include "Logger.h"

using namespace std;

// checks if a key is pressed and waits for it to be released before return
bool check_glfw_key(GLFWwindow *window, int key) {
    if (glfwGetKey(window, key) == GLFW_PRESS) {
        while (glfwGetKey(window, key) == GLFW_PRESS) {
            usleep(10000);
            glfwPollEvents();
        }
        return true;
    }
    return false;
}

void process_file(Logger &log, const string& executable_dir, InputReader &reader,
                  bool decode_all_fields, bool full_screen, bool no_sync,
                  bool start_paused, bool decode_video, bool decode_audio) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                          "MUSE", full_screen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);

    musevk::VulkanManager manager(log);
    manager.initVulkan(window, no_sync);
    vk::Device &device = manager.getDevice();

    {
        std::vector<std::shared_ptr<musevk::VulkanBuffer>> input_vulkan_buffers{};
        for (int i = 0; i < 5; i++)
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

    AudioPlayback audio_playback(log);

    AudioDecoder::AudioMode audio_mode;
    size_t audio_sample_count;
    AudioDecoder::AudioFrame audio_samples[AudioDecoder::c_max_output_samples];

    {
        auto queue = manager.createCommandQueue(
                std::vector{vk::Semaphore(image_available_semaphore)},
                std::vector{vk::PipelineStageFlags(vk::PipelineStageFlagBits::eBottomOfPipe)});
        Shaders shaders(log, executable_dir, manager);
        auto decoder = MuseDecoder(log,
                                   reader,
                                   shaders,
                                   manager,
                                   decode_video,
                                   decode_all_fields,
                                   decode_audio,
                                   false); // benchmark_shaders
        if (!decoder.initialize())
            throw runtime_error("MuseDecoder initialization failed");
        auto image = shaders.getResultImage();

        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        bool paused = false;
        int paused_countdown = start_paused ? 5 : 0;
        MuseDecoder::FieldInterpolationMode field_interpolation_mode = MuseDecoder::eNormal;
        bool redo_last_field = false;

        // We skip calling next every other field if decode_all_fields is false
        while (paused || decoder.next(audio_mode, audio_sample_count, audio_samples, field_interpolation_mode, redo_last_field)) {
            if (!paused)
                field_count++;
            redo_last_field = false;

            if (audio_sample_count != 0 && audio_mode != AudioDecoder::MODE_UNKNOWN && !paused)
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

            if (paused_countdown != 0 && --paused_countdown == 0) {
                paused = true;
            }
            if (glfwWindowShouldClose(window))
                break;
            glfwPollEvents();

            if (check_glfw_key(window, GLFW_KEY_Q))
                break;
            if (check_glfw_key(window, GLFW_KEY_ESCAPE) && full_screen) {
                glfwSetWindowMonitor(window, nullptr, 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                full_screen = false;
            }
            if (check_glfw_key(window, GLFW_KEY_TAB) && !full_screen) {
                glfwSetWindowMonitor(window, glfwGetPrimaryMonitor(), 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                full_screen = true;
            }
            if (check_glfw_key(window, GLFW_KEY_SPACE))
                paused = !paused;
            if (check_glfw_key(window, GLFW_KEY_RIGHT)) {
                paused = false;
                paused_countdown = 1;
            }
            if (check_glfw_key(window, GLFW_KEY_1)) {
                field_interpolation_mode = MuseDecoder::eNormal;
                if (paused) {
                    paused = false;
                    redo_last_field = true;
                    paused_countdown = 1;
                }
                log.info(eApplication | eVideo | eDecoder, "Field interpolation determined by motion detection");
            }
            if (check_glfw_key(window, GLFW_KEY_2)) {
                field_interpolation_mode = MuseDecoder::eForceIntraField;
                if (paused) {
                    paused = false;
                    redo_last_field = true;
                    paused_countdown = 1;
                }
                log.info(eApplication | eVideo | eDecoder, "Field interpolation forced to intra field only");
            }
            if (check_glfw_key(window, GLFW_KEY_3)) {
                field_interpolation_mode = MuseDecoder::eForceInterFrame;
                if (paused) {
                    paused = false;
                    redo_last_field = true;
                    paused_countdown = 1;
                }
                log.info(eApplication | eVideo | eDecoder, "Inter-frame interpolation forced");
            }
        }
        auto t1 = chrono::high_resolution_clock::now();
        auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        log.info(eApplication | ePerformance,
            fmt::format("Avg {:.3f} ms/frame ({:.3f} frames/s)",
                        time_us / 1000.0 / field_count * 2,
                        1000000.0 / time_us * field_count / 2));
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
    cerr << "usage: musecpp "
            "<--resample-bytes|--resample-shorts|--big-endian> [--sample-freq] "
            "[--fifo] [--full-frames-only] [-all-fields] "
            "[--full-screen] [--no-video] [no-audio] [--verbose] [--nosync] [--help] "
            "<input_file> ...\n";
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    auto log_selection = Logger::c_log_warn;
    enum InputFormat {
        eOverSampledUnsignedBytes,
        eOverSampledSignedShortsLittleEndian,
        eLittleEndianShorts,
        eBigEndianShorts,
        eUnknown
    };
    std::string executable(argv[0]);
    std::string executable_dir = executable.substr(0, executable.find_last_of('/'));
    bool decode_all_fields = true;
    bool full_screen = false;
    bool no_sync = false;
    InputFormat input_format = eUnknown;
    double input_sample_frequency = 40e6;
    bool input_is_fifo = false;
    double initial_seek_seconds = 0;
    bool start_paused = false;
    optional<string> output_filename; // always written as little endian unsigned short values
    bool decode_video = true;
    bool decode_audio = true;

    try {
        const vector<string> args(argv + 1, argv + argc);
        for (auto it = args.cbegin(), end = args.cend(); it != end; it++) {
            if (*it == "--resample-bytes")
                input_format = eOverSampledUnsignedBytes;
            else if (*it == "--resample-shorts")
                input_format = eOverSampledSignedShortsLittleEndian;
            else if (*it == "--sample-freq")
                input_sample_frequency = stod(*(++it));
            else if (*it == "--little-endian")
                input_format = eLittleEndianShorts;
            else if (*it == "--big-endian")
                input_format = eBigEndianShorts;
            else if (*it == "--fifo")
                input_is_fifo = true;
            else if (*it == "--full-frames-only")
                decode_all_fields = false;
            else if (*it == "--all-fields")
                decode_all_fields = true;
            else if (*it == "--full-screen")
                full_screen = true;
            else if (*it == "--seek")
                initial_seek_seconds = stod(*(++it));
            else if (*it == "--pause")
                start_paused = true;
            else if (*it == "--write")
                output_filename = *(++it);
            else if (*it == "--no-video")
                decode_video = false;
            else if (*it == "--no-audio")
                decode_audio = false;
            else if (*it == "--verbose")
                log_selection = Logger::c_log_all;
            else if (*it == "--no-sync")
                no_sync = true;
            else if (*it == "--help")
                usage();
            else {
                if (initial_seek_seconds != 0 && input_is_fifo)
                    throw runtime_error("Initial seek is not compatible with reading from fifo");
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                Logger log(log_selection);
//                Logger log({{eInput, eDebug}, {eAudio, eWarn}, {eVideo, eInfo}});
                InputReader *reader;
                switch (input_format) {
                    case eOverSampledUnsignedBytes:
                    case eOverSampledSignedShortsLittleEndian:
                        reader = new ResamplingInputReader(
                                log, *it,
                                input_format == eOverSampledSignedShortsLittleEndian ? ResamplingInputReader::eSignedShortLittleEndian : ResamplingInputReader::eUnsignedByte,
                                input_sample_frequency, input_is_fifo, initial_seek_seconds,
                                output_filename);
                        break;
                    case eLittleEndianShorts:
                    case eBigEndianShorts:
                        reader = new PhaseCorrect16MHzInputReader(log, *it,
                                                                  input_format == eBigEndianShorts,
                                                                  input_is_fifo, initial_seek_seconds,
                                                                  output_filename);
                        break;
                    case eUnknown:
                    default:
                        throw runtime_error("No input format specified");
                }
                process_file(log, executable_dir, *reader, decode_all_fields,
                             full_screen, no_sync, start_paused, decode_video, decode_audio);
            }
        }
    } catch (const exception &x) {
        Logger log(Logger::c_log_all);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return 0;
}
