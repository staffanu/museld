#include <filesystem>
#include <fmt/format.h>
#include <set>
#include "Shaders.h"
#include "MuseDecoder.h"
#include "musevk/VulkanManager.h"
#include "MuseTypes.h"
#include "AudioPlayback.h"
#include "InputReader.h"
#include "ResamplingInputReader.h"
#include "PhaseCorrect16MHzInputReader.h"
#include "Logger.h"
#include "musevk/TimestampQueryPool.h"

#define INPUT_BUFFER_COUNT 6

using namespace std;

static set<int> current_keys_down;

bool check_glfw_key(GLFWwindow *window, int key) {
    if (glfwGetKey(window, key) == GLFW_PRESS) {
        if (current_keys_down.find(key) == current_keys_down.end()) {
            current_keys_down.insert(key);
            return true;
        } else
            return false;
    } else {
        current_keys_down.erase(key);
        return false;
    }
}

void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "Error %d: %s\n", error, description); // FIXME: use logging framework
}

void process_file(Logger &log, const string& executable_dir, InputReader &reader,
                  bool decode_all_fields, bool full_screen, bool no_sync,
                  bool start_paused, bool decode_video, bool decode_audio, bool efm_audio, bool benchmark_shaders) {
    glfwSetErrorCallback(glfw_error_callback);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                          "MUSE", full_screen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

    musevk::VulkanManager manager(log);
    manager.initVulkan(window, no_sync);
    musevk::TimestampQueryPool *timestamp_query_pool =
            benchmark_shaders ? new musevk::TimestampQueryPool(manager.getPhysicalDevice(), manager.getDevice(), 40) : nullptr;
    vk::Device &device = manager.getDevice();

    {
        std::vector<std::shared_ptr<musevk::VulkanBuffer>> input_vulkan_buffers{};
        for (int i = 0; i < INPUT_BUFFER_COUNT; i++)
            input_vulkan_buffers.push_back(
                    manager.createBuffer(MUSE_TOTAL_HEIGHT * MUSE_TOTAL_WIDTH, sizeof(float), true, true));
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

    AudioMode audio_mode;
    size_t audio_sample_count;
    AudioDecoder::AudioFrame audio_samples[AudioDecoder::c_max_output_samples];

    {
        auto command_buffer = manager.createCommandBuffer();
        Shaders shaders(log, executable_dir, manager);
        auto decoder = MuseDecoder(log,
                                   reader,
                                   shaders,
                                   manager,
                                   decode_video,
                                   decode_all_fields,
                                   decode_audio,
                                   timestamp_query_pool);
        if (!decoder.initialize())
            throw runtime_error("MuseDecoder initialization failed");
        auto image = shaders.getResultImage();

        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        bool paused = false;
        int paused_countdown = start_paused ? 5 : 0;
        MuseDecoder::FieldInterpolationMode field_interpolation_mode = MuseDecoder::eNormal;
        bool redo_last_field = false;
        bool enable_non_linear = true;

        while (paused || decoder.next(efm_audio, audio_mode, audio_sample_count, audio_samples, field_interpolation_mode, redo_last_field, enable_non_linear)) {
            if (!paused)
                field_count++;
            redo_last_field = false;

            if (audio_sample_count != 0 && audio_mode != MODE_UNKNOWN && !paused)
                audio_playback.add_samples(audio_mode, audio_sample_count, audio_samples);

            //vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
            //vkResetFences(device, 1, &in_flight_fence);

            auto swap_chain_image = manager.acquireNextImage(image_available_semaphore);

            // The output image written by the decoder is finished since the next() call waits for
            // all commands to finish.

            command_buffer->begin();
            image->enqueueTransitionLayout(*command_buffer, vk::ImageLayout::eTransferSrcOptimal,
                                           vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                                           vk::AccessFlags(), vk::AccessFlagBits::eTransferRead);
            command_buffer->enqueueTransitionMemoryLayout(swap_chain_image,
                                                          vk::ImageLayout::eUndefined,
                                                          vk::ImageLayout::eTransferDstOptimal,
                                                          vk::PipelineStageFlagBits::eTopOfPipe,
                                                          vk::PipelineStageFlagBits::eTransfer,
                                                          vk::AccessFlags(),
                                                          vk::AccessFlagBits::eTransferWrite);

            auto extent = manager.getSwapChainExtent();
            vk::ImageBlit region;
            region.srcOffsets[0] = vk::Offset3D(0, 0, 0);
            region.srcOffsets[1] = vk::Offset3D(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 1);
            region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            region.dstOffsets[1] = vk::Offset3D((int)extent.width, (int)extent.height, 1);
            region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            command_buffer->enqueueBlitImage(image->image(), vk::ImageLayout::eTransferSrcOptimal,
                                             swap_chain_image, vk::ImageLayout::eTransferDstOptimal,
                                             region);

            command_buffer->enqueueTransitionMemoryLayout(swap_chain_image,
                                                          vk::ImageLayout::eTransferDstOptimal,
                                                          vk::ImageLayout::ePresentSrcKHR,
                                                          vk::PipelineStageFlagBits::eTransfer,
                                                          vk::PipelineStageFlagBits::eBottomOfPipe,
                                                          vk::AccessFlagBits::eTransferWrite,
                                                          vk::AccessFlags());
            command_buffer->submit({image_available_semaphore}, {vk::PipelineStageFlagBits::eTopOfPipe}, {});
            command_buffer->wait();

            manager.present(swap_chain_image);

            if (paused_countdown != 0 && --paused_countdown == 0) {
                paused = true;
            }
            if (glfwWindowShouldClose(window))
                break;
            glfwPollEvents();

            if (check_glfw_key(window, GLFW_KEY_ESCAPE) || check_glfw_key(window, GLFW_KEY_Q))
                break;
            if (check_glfw_key(window, GLFW_KEY_TAB)) {
                if (full_screen) {
                    glfwSetWindowMonitor(window, nullptr, 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                    full_screen = false;
                } else {
                    glfwSetWindowMonitor(window, glfwGetPrimaryMonitor(), 0, 0, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, 60);
                    full_screen = true;
                }
            }
            if (check_glfw_key(window, GLFW_KEY_SPACE))
                paused = !paused;
            if (check_glfw_key(window, GLFW_KEY_N)) {
                paused = false;
                paused_countdown = 1;
            }
            if (check_glfw_key(window, GLFW_KEY_LEFT)) {
                reader.seek(-10);
                if (paused) {
                    paused = false;
                    paused_countdown = 5;
                }
            }
            if (check_glfw_key(window, GLFW_KEY_RIGHT)) {
                reader.seek(10);
                if (paused) {
                    paused = false;
                    paused_countdown = 5;
                }
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
            if (check_glfw_key(window, GLFW_KEY_A)) {
                efm_audio = !efm_audio;
            }
            if (check_glfw_key(window, GLFW_KEY_E)) {
                enable_non_linear = true;
            }
            if (check_glfw_key(window, GLFW_KEY_L)) {
                enable_non_linear = false;
            }
        }
        auto t1 = chrono::high_resolution_clock::now();
        auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        log.info(eApplication | ePerformance,
            fmt::format("Total {} frames.  Avg {:.3f} ms/m_frame ({:.3f} frames/s)",
                        field_count / 2,
                        time_us / 1000.0 / field_count * 2,
                        1000000.0 / time_us * field_count / 2));
        if (benchmark_shaders)
            decoder.output_benchmark_results();
    }

    delete timestamp_query_pool;
    device.destroy(image_available_semaphore);
    device.destroy(render_finished_semaphore);
    device.destroy(in_flight_fence);

    audio_playback.cleanup();
    reader.cleanup();
    manager.cleanup();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void usage() {
    cerr << "usage: musecpp "
            "<--resample-bytes|--resample-shorts|--big-endian> [--sample-freq] "
            "[--fifo] [--full-frames-only] [-all-fields] "
            "[--full-screen] [--no-video] [no-audio] [--verbose] [--no-sync] [--help] "
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
    bool demodulate = false;
    InputFormat input_format = eUnknown;
    double input_sample_frequency = 40e6;
    bool input_is_fifo = false;
    double initial_seek_seconds = 0;
    bool start_paused = false;
    optional<string> output_filename; // always written as little endian unsigned short values
    bool decode_video = true;
    bool decode_audio = true;
    bool efm_audio = false;
    bool benchmark_shaders = false;

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
            else if (*it == "--demodulate")
                demodulate = true;
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
            else if (*it == "--efm")
                efm_audio = true;
            else if (*it == "--verbose")
                log_selection = Logger::c_log_all;
            else if (*it == "--benchmark-shaders")
                benchmark_shaders = true;
            else if (*it == "--no-sync")
                no_sync = true;
            else if (*it == "--help")
                usage();
            else if (it -> find("!", 0) == 0)
                ; // used to ignore options (to easily enable/disable options in debug settings etc.)
            else {
                if (initial_seek_seconds != 0 && input_is_fifo)
                    throw runtime_error("Initial seek is not compatible with reading from fifo");
                if (!filesystem::exists(*it))
                    throw runtime_error("File not found: " + string(*it));
                Logger log(log_selection);
//                Logger log({{eInput, eWarn}, {eAudio, eWarn}, {eVideo, eWarn}, {ePerformance, eWarn}});


                if (demodulate)
                    input_format = eOverSampledSignedShortsLittleEndian;
                InputReader *reader;
                switch (input_format) {
                    case eOverSampledUnsignedBytes:
                    case eOverSampledSignedShortsLittleEndian:
                        reader = new ResamplingInputReader(
                                log, *it,
                                input_format == eOverSampledSignedShortsLittleEndian ? ResamplingInputReader::eSignedShortLittleEndian : ResamplingInputReader::eUnsignedByte,
                                input_sample_frequency, initial_seek_seconds, demodulate, output_filename);
                        break;
                    case eLittleEndianShorts:
                    case eBigEndianShorts:
                        reader = new PhaseCorrect16MHzInputReader(log, *it,
                                                                  input_format == eBigEndianShorts,
                                                                  initial_seek_seconds, output_filename);
                        break;
                    case eUnknown:
                    default:
                        throw runtime_error("No input format specified");
                }
                process_file(log, executable_dir, *reader, decode_all_fields,
                             full_screen, no_sync, start_paused, decode_video, decode_audio, efm_audio,
                             benchmark_shaders);
            }
        }
    } catch (const exception &x) {
        Logger log(Logger::c_log_all);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return 0;
}
