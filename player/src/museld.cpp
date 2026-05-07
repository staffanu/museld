#include <filesystem>
#include <format>
#include <set>
#include <functional>
#include "musevk/TimestampQueryPool.h"
#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"
#include "logging/StreamLogger.h"
#include "AudioPlayback.h"
#include "FrameReader.h"
#include "TextRenderer.h"
#include "Decoder.h"
#include "input/InputReader.h"
#include "input/InputReaderFactory.h"
#include "muse/ResamplingFrameReader.h"
#include "muse/PhaseCorrect16MHzFrameReader.h"
#include "muse/MuseDecoder.h"
#include "muse/MuseConstants.h"
#include "ntsc/NtscConstants.h"
#include "ntsc/NtscFrameReader.h"
#include "ntsc/NtscDecoder.h"

#ifdef HAVE_LIBAV
# include "VideoFileWriter.h"
#endif

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

template<class InputBlock>
void process_file(Logger &log, const string &executable_dir, musevk::VulkanManager &manager, FrameReader<InputBlock> &reader,
                  bool decode_all_fields, bool full_screen, bool no_sync,
                  bool start_paused, bool decode_video, DropoutMode dropout_mode,
                  bool decode_audio, bool efm_audio, bool benchmark_shaders,
                  optional<string> const &output_filename) {
    glfwSetErrorCallback(glfw_error_callback);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *window = glfwCreateWindow(MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2,
                                          "MUSE", full_screen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

    manager.initVulkan(window, no_sync);
    musevk::TimestampQueryPool *timestamp_query_pool =
            benchmark_shaders ? new musevk::TimestampQueryPool(manager.getPhysicalDevice(), manager.getDevice(), 40) : nullptr;
    vk::Device &device = manager.getDevice();

    {
        std::vector<std::unique_ptr<InputBlock>> input_vulkan_buffers{};
        for (int i = 0; i < INPUT_BUFFER_COUNT; i++)
            input_vulkan_buffers.push_back(InputBlockFactory<InputBlock>::makeBlock(manager));
        if (!reader.initialize(input_vulkan_buffers))
            throw runtime_error("FrameReader initialization failed");
    }

#ifdef HAVE_LIBAV
    unique_ptr<VideoFileWriter> vfw = nullptr;
    if (output_filename) {
        vfw = make_unique<VideoFileWriter>(output_filename.value(), log, MUSE_Y_BUF_WIDTH * 3, MUSE_BUF_HEIGHT * 2, decode_all_fields ? 60 : 30);
        if (!vfw->init())
            throw runtime_error("Cannot initialize output encoder");
    }
#endif

    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{};
    fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    auto image_available_semaphore = device.createSemaphore(semaphoreInfo);
    auto render_finished_semaphore = device.createSemaphore(semaphoreInfo);
    auto in_flight_fence = device.createFence(fenceInfo);

    unique_ptr<AudioPlayback> audio_playback = nullptr;
    if (decode_audio)
         audio_playback = make_unique<AudioPlayback>(log);

    AudioMode audio_mode;
    int audio_sample_count;
    AudioFrame audio_samples[MAX_AUDIO_OUTPUT_SAMPLES];

    {
        musevk::CommandPool command_pool(manager);
        auto command_buffer = command_pool.createCommandBuffer();
        TextRenderer text_renderer(executable_dir, manager, command_pool);

        std:unique_ptr<Decoder> decoder = nullptr;
        if (std::is_same<InputBlock, MuseInputBlock>::value) {
            decoder = std::make_unique<MuseDecoder>(log,
                                                    (FrameReader<MuseInputBlock> &)reader,
                                                    manager,
                                                    command_pool,
                                                    executable_dir,
                                                    decode_video,
                                                    decode_all_fields,
                                                    decode_audio,
                                                    timestamp_query_pool);
        } else {
            decoder = std::make_unique<NtscDecoder>(log,
                                                    (FrameReader<NtscInputBlock> &)reader,
                                                    manager,
                                                    command_pool,
                                                    executable_dir,
                                                    decode_video,
                                                    decode_all_fields,
                                                    decode_audio,
                                                    timestamp_query_pool);
        }

        if (!decoder->initialize())
            throw runtime_error("MuseDecoder initialization failed");
        auto images = decoder->getResultImages();

        auto t0 = chrono::high_resolution_clock::now();
        int field_count = 0;
        bool paused = false;
        int paused_countdown = start_paused ? 5 : 0;
        Decoder::FieldInterpolationMode field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        bool redo_last_field = false;
        bool enable_non_linear = true;
        bool enable_cursor = false;
        bool show_disc_code = false;
        int zoom_factor = 1;
        const double zoom_step = 0.2;
        pair<double, double> zoom_center = make_pair(0.5, 0.5);
        string osd_text;
        string prev_osd_text;
        int osd_text_remaining_frames = 0;
        double input_samples_per_muse_sample;
        long last_buffer_file_offset;
        std::shared_ptr<DiscInfo> disc_info = nullptr;
        int field_parity;

        while (paused && !redo_last_field ||
            decoder->next(efm_audio, &audio_mode, &audio_sample_count, audio_samples,
                         &field_parity, &last_buffer_file_offset, &input_samples_per_muse_sample, &disc_info,
                         field_interpolation_mode, redo_last_field, enable_non_linear, dropout_mode, output_filename.has_value())) {

            if (!paused)
                field_count++;
            redo_last_field = false;

#ifdef HAVE_LIBAV
            if (vfw != nullptr)
                vfw->addVideoFrameWithAudio(images.out_Y, images.out_U, images.out_V, audio_mode, audio_sample_count, audio_samples);
#endif

            if (audio_sample_count != 0 && audio_mode != MODE_UNKNOWN && !paused)
                audio_playback->add_samples(audio_mode, audio_sample_count, audio_samples);

            //vkWaitForFences(device, 1, &in_flight_fence, VK_TRUE, UINT64_MAX);
            //vkResetFences(device, 1, &in_flight_fence);

            auto swap_chain_image = manager.acquireNextImage(image_available_semaphore);

            // The output image written by the decoder is finished since the next() call waits for
            // all commands to finish.

            command_buffer->begin();

            if (osd_text != prev_osd_text) {
                if (paused && osd_text_remaining_frames)
                    redo_last_field = true;
                osd_text_remaining_frames = 100;
                prev_osd_text = osd_text;
            }
            if (osd_text_remaining_frames) {
                text_renderer.drawText(images.out_image, 90, 50, osd_text, 4, *command_buffer);
                if (!--osd_text_remaining_frames)
                    redo_last_field = true;
            }
            string cursor_string;
            if (enable_cursor) {
                // Draw the pointer coordinates in the top left corner (both in single field and interpolated coordinates)
                // If paused, and showing only a single field, also show the input stream offset for the start of the frame,
                // the start of the current field (first line of audio data section), and the offsets for the pixel under the
                // pointer, in the Y, Cr and Cb data. The offsets unfortunately aren't exact (I don't know why), but the
                // offsets are very useful when investigating dropouts.
                int xsize, ysize;
                glfwGetWindowSize(window, &xsize, &ysize);
                double xpos, ypos;
                glfwGetCursorPos(window, &xpos, &ypos);
                if (xpos >= 0 && ypos >= 0 && xpos < xsize && ypos < ysize) {
                    double rel_x = (xpos / xsize - 0.5) / zoom_factor + zoom_center.first;
                    double rel_y = (ypos / ysize - 0.5) / zoom_factor + zoom_center.second;
                    int field_x = (int)(rel_x * MUSE_Y_BUF_WIDTH);
                    int field_y = (int)(rel_y * MUSE_BUF_HEIGHT);
                    cursor_string = std::format("({}, {}) ({}, {})",
                                                (int)(rel_x * MUSE_Y_BUF_WIDTH * 3),
                                                (int)(rel_y * MUSE_BUF_HEIGHT * 2),
                                                field_x, field_y);
                    text_renderer.drawText(images.out_image, 10, 8, cursor_string, 1, *command_buffer);
                    if (field_interpolation_mode == Decoder::FieldInterpolationMode::eForceIntraField && paused) {
                        long field_offset = last_buffer_file_offset // start of sound data
                                + (long)((field_parity ? 565 : 2) * MUSE_TOTAL_WIDTH * input_samples_per_muse_sample);
                        string offset_string =
                                std::format("{} {} {} Y {} Cr {} Cb {}",
                                            last_buffer_file_offset,
                                            field_parity ? "ODD" : "EVEN",
                                            field_offset,
                                            field_offset + (int)(input_samples_per_muse_sample * ((field_y + 44) * MUSE_TOTAL_WIDTH + (field_x + 106))),
                                            field_offset + (int)(input_samples_per_muse_sample * ((field_y + 40) / 2 * 2) * MUSE_TOTAL_WIDTH + field_x / 4 + 11),
                                            field_offset + (int)(input_samples_per_muse_sample * ((field_y + 40) / 2 * 2 + 1) * MUSE_TOTAL_WIDTH + field_x / 4 + 11));
                        text_renderer.drawText(images.out_image, 10, 34, offset_string, 1, *command_buffer);
                        cursor_string += " " + offset_string;
                    }
                }
                if (paused)
                    redo_last_field = true;
            }
            if (show_disc_code) {
                auto disc_info_strings = disc_info ? disc_info->asStrings() : std::vector<std::string>{"No disc info"};
                for (int i = disc_info_strings.size() - 1, y = 955; i >= 0; i--, y -= 55) {
                    text_renderer.drawText(images.out_image, 90, y, disc_info_strings[i], 2, *command_buffer);
                }
                if (paused)
                    redo_last_field = true;
            }

            images.out_image->enqueueTransitionLayout(*command_buffer, vk::ImageLayout::eTransferSrcOptimal,
                                           vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                                           vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
            command_buffer->enqueueTransitionMemoryLayout(swap_chain_image,
                                                          vk::ImageLayout::eUndefined,
                                                          vk::ImageLayout::eTransferDstOptimal,
                                                          vk::PipelineStageFlagBits::eTopOfPipe,
                                                          vk::PipelineStageFlagBits::eTransfer,
                                                          vk::AccessFlags(),
                                                          vk::AccessFlagBits::eTransferWrite);

            auto extent = manager.getSwapChainExtent();
            int source_width = std::is_same<InputBlock, MuseInputBlock>::value ? MUSE_Y_BUF_WIDTH * 3 : NTSC_Y_BUF_WIDTH;
            int source_height = std::is_same<InputBlock, MuseInputBlock>::value ? MUSE_BUF_HEIGHT * 2 : NTSC_FIELD_HEIGHT * 2;
            vk::ImageBlit region;
            region.srcOffsets[0] = vk::Offset3D((int32_t)((zoom_center.first - 0.5 / zoom_factor) * source_width),
                                                (int32_t)((zoom_center.second - 0.5 / zoom_factor) * source_height), 0);
            region.srcOffsets[1] = vk::Offset3D((int32_t)((zoom_center.first + 0.5 / zoom_factor) * source_width),
                                                (int32_t)((zoom_center.second + 0.5 / zoom_factor) * source_height), 1);
            region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            region.dstOffsets[1] = vk::Offset3D((int) extent.width, (int) extent.height, 1);
            region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            command_buffer->enqueueBlitImage(images.out_image->image(), vk::ImageLayout::eTransferSrcOptimal,
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
                osd_text = "PAUSE";
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
            if (check_glfw_key(window, GLFW_KEY_SPACE)) {
                paused = !paused;
                osd_text = paused ? "PAUSE" : "PLAY";
            }
            if (check_glfw_key(window, GLFW_KEY_N)) {
                paused = false;
                redo_last_field = false;
                paused_countdown = 1;
            }
            if (check_glfw_key(window, GLFW_KEY_LEFT)) {
                if (zoom_factor != 1)
                    zoom_center.first = max(0.5 / zoom_factor, zoom_center.first - zoom_step / zoom_factor);
                else {
                    reader.seek(-10);
                    if (paused) {
                        paused = false;
                        paused_countdown = 5;
                    }
                }
            }
            if (check_glfw_key(window, GLFW_KEY_RIGHT)) {
                if (zoom_factor != 1)
                    zoom_center.first = min(1.0 - 0.5 / zoom_factor, zoom_center.first + zoom_step / zoom_factor);
                else {
                    reader.seek(10);
                    if (paused) {
                        paused = false;
                        paused_countdown = 5;
                    }
                }
            }
            if (check_glfw_key(window, GLFW_KEY_UP)) {
                zoom_center.second = max(0.5 / zoom_factor, zoom_center.second - zoom_step / zoom_factor);
            }
            if (check_glfw_key(window, GLFW_KEY_DOWN)) {
                zoom_center.second = min(1.0 - 0.5 / zoom_factor, zoom_center.second + zoom_step / zoom_factor);
            }
            if (check_glfw_key(window, GLFW_KEY_1)) {
                field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
                if (paused)
                    redo_last_field = true;
                log.info(eApplication | eVideo, "Field interpolation determined by motion detection");
                osd_text = "MOTION NORMAL";
            }
            if (check_glfw_key(window, GLFW_KEY_2)) {
                field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
                if (paused)
                    redo_last_field = true;
                log.info(eApplication | eVideo, "Field interpolation forced to intra field only");
                osd_text = "MOTION ALL";
            }
            if (check_glfw_key(window, GLFW_KEY_3)) {
                field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
                if (paused)
                    redo_last_field = true;
                log.info(eApplication | eVideo, "Inter-frame interpolation forced");
                osd_text = "MOTION NONE";
            }
            if (check_glfw_key(window, GLFW_KEY_A)) {
                efm_audio = !efm_audio;
                reader.setEfmEnabled(efm_audio);
                osd_text = efm_audio ? "EFM AUDIO" : "MUSE AUDIO";
            }
            if (check_glfw_key(window, GLFW_KEY_D)) {
                switch (dropout_mode) {
                    case DropoutMode::eNormal:
                        dropout_mode = DropoutMode::eDisabled;
                        osd_text = "DROPOUT DISABLED";
                        break;
                    case DropoutMode::eDisabled:
                        dropout_mode = DropoutMode::eHighlight;
                        osd_text = "DROPOUT HIGHLIGHT";
                        break;
                    case DropoutMode::eHighlight:
                        dropout_mode = DropoutMode::eNormal;
                        osd_text = "DROPOUT ENABLED";
                        break;
                }
            }
            if (check_glfw_key(window, GLFW_KEY_L)) {
                enable_non_linear = !enable_non_linear;
                osd_text = enable_non_linear ? "NON-LINEAR DE-EMPH ON" : "NON-LINEAR DE-EMPH OFF";
            }
            if (check_glfw_key(window, GLFW_KEY_C)) {
                enable_cursor = !enable_cursor;
                glfwSetInputMode(window, GLFW_CURSOR, enable_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
            }
            if (check_glfw_key(window, GLFW_KEY_V)) {
                show_disc_code = !show_disc_code;
            }
            if (check_glfw_key(window, GLFW_KEY_PRINT_SCREEN)) {
                glfwSetClipboardString(window, cursor_string.c_str());
            }
            if (check_glfw_key(window, GLFW_KEY_Z)) {
                zoom_factor = (zoom_factor * 2) % 7;
                zoom_center.first = max(0.5 / zoom_factor, min(1.0 - 0.5 / zoom_factor, zoom_center.first));
                zoom_center.second = max(0.5 / zoom_factor, min(1.0 - 0.5 / zoom_factor, zoom_center.second));
                osd_text = std::format("ZOOM {}", zoom_factor);
            }
        }
        auto t1 = chrono::high_resolution_clock::now();
        auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        log.info(eApplication | ePerformance,
            std::format("Total {} frames.  Avg {:.3f} ms/frame ({:.3f} frames/s)",
                        field_count / 2,
                        time_us / 1000.0 / field_count * 2,
                        1000000.0 / time_us * field_count / 2));
        if (benchmark_shaders)
            decoder->outputBenchmarkResults();
        decoder = nullptr;
    }

    delete timestamp_query_pool;
    device.destroy(image_available_semaphore);
    device.destroy(render_finished_semaphore);
    device.destroy(in_flight_fence);

#ifdef HAVE_LIBAV
    if (vfw != nullptr) {
        vfw->cleanup();
        vfw = nullptr;
    }
#endif
    if (audio_playback != nullptr)
        audio_playback->cleanup();
    reader.cleanup();
    manager.cleanup();

    glfwDestroyWindow(window);
    glfwTerminate();
}

enum InputType {
    eMuseRf,
    eNtscRf,
    eMuse16MHz,
    eMuseOversampled,
};

int main(int argc, char *argv[]) {
    auto log_selection = StreamLogger::c_log_warn;
    std::string executable(argv[0]);
    std::string executable_dir = executable.substr(0, executable.find_last_of('/'));
    bool decode_all_fields = true;
    bool full_screen = false;
    bool no_sync = false;
    InputType input_type = eMuseRf;
    std::optional<InputFormat> input_format_option = std::nullopt;
    double input_sample_frequency = 62.5e6;
    double initial_seek_seconds = 0;
    bool start_paused = false;
    optional<string> muse_output_filename; // always written as little endian unsigned short values
    optional<string> output_filename; // format selected automatically from filename
    bool decode_video = true;
    DropoutMode dropout_mode = DropoutMode::eNormal;
    bool decode_audio = true;
    bool efm_audio = false;
    bool benchmark_shaders = false;

    const vector<string> args(argv + 1, argv + argc);
    auto it = args.cbegin();

    vector<pair<string, function<void ()>>> options;

    auto usage = [&options] () -> void {
        cerr << "usage: museld ";
        for (auto o: options)
            cerr << "[" << o.first << "] ";
        cerr << "<input_file> ..." << endl;
        exit(EXIT_FAILURE);
    };

    options.emplace_back("--input-format", [&] () mutable -> void {
        input_format_option = inputFormatFromString(*(it++));
    });
    options.emplace_back("--sample-freq", [&] () mutable  -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.emplace_back("--input-type", [&] () mutable -> void {
        const auto &name = *(it++);
        if      (name == "muse-rf")  input_type = eMuseRf;
        else if (name == "ntsc-rf")  input_type = eNtscRf;
        else if (name == "muse-16")  input_type = eMuse16MHz;
        else if (name == "muse-os")  input_type = eMuseOversampled;
        else throw std::runtime_error(std::format("Unknown input type {}", name));
    });
    options.emplace_back("--full-frames-only", [&] () mutable -> void {
        decode_all_fields = false;
    });
    options.emplace_back("--all-fields", [&] () mutable -> void {
        decode_all_fields = true;
    });
    options.emplace_back("--full-screen", [&] () mutable -> void {
        full_screen = true;
    });
    options.emplace_back("--seek", [&] () mutable -> void {
        initial_seek_seconds = stod(*(it++));
    });
    options.emplace_back("--pause", [&] () mutable -> void {
        start_paused = true;
    });
    options.emplace_back("--write-muse16", [&] () mutable -> void {
        muse_output_filename = *(it++);
    });
    options.emplace_back("--write", [&] () mutable -> void {
#ifdef HAVE_LIBAV
        output_filename = *(it++);
#else
        throw std::runtime_error("FFMPEG is not available");
#endif
    });
    options.emplace_back("--no-video", [&] () mutable -> void {
        decode_video = false;
    });
    options.emplace_back("--no-dropout", [&] () mutable -> void {
        dropout_mode = DropoutMode::eDisabled;
    });
    options.emplace_back("--highlight-dropout", [&] () mutable -> void {
        dropout_mode = DropoutMode::eHighlight;
    });
    options.emplace_back("--no-audio", [&] () mutable -> void {
        decode_audio = false;
    });
    options.emplace_back("--efm", [&] () mutable -> void {
        efm_audio = true;
    });
    options.emplace_back("--log", [&] () mutable -> void {
        // Logging is specified with a string with a letter corresponding to the category
        // (MPAVDIO for Main(Application), Performance, Audio, Video, Decoder, Input, and Output, respectively,
        // and a number corresponding to the amount of logging. 0-4 imples Off, Error, Warn, Info, Debug.
        // Default is Warn for all categories.
        if (it->length() %2)
            throw runtime_error("Log specification should have even length");
        auto parseLevel = [](char c) -> LogPriority {
            switch (c) {
                case '0': return eOff;
                case '1': return eError;
                case '2': return eWarn;
                case '3': return eInfo;
                case '4': return eDebug;
                default: throw runtime_error("Invalid log level");
            }
        };
        for (int i = 0; i < it->length(); i += 2) {
            switch ((*it)[i]) {
                case 'M':
                    log_selection[eApplication] = parseLevel((*it)[i + 1]);
                    break;
                case 'P':
                    log_selection[ePerformance] = parseLevel((*it)[i + 1]);
                    break;
                case 'A':
                    log_selection[eAudio] = parseLevel((*it)[i + 1]);
                    break;
                case 'V':
                    log_selection[eVideo] = parseLevel((*it)[i + 1]);
                    break;
                case 'D':
                    log_selection[eDecoder] = parseLevel((*it)[i + 1]);
                    break;
                case 'I':
                    log_selection[eInput] = parseLevel((*it)[i + 1]);
                    break;
                case 'O':
                    log_selection[eOutput] = parseLevel((*it)[i + 1]);
                    break;
                default:
                    throw runtime_error("Unknown log category");
            }
        }
        it++;
    });
    options.emplace_back("--benchmark-shaders", [&] () mutable -> void {
        benchmark_shaders = true;
    });
    options.emplace_back("--no-sync", [&] () mutable -> void {
        no_sync = true;
    });
    options.emplace_back("--help", [&] () mutable -> void {
        usage();
    });

    try {
        while (it != args.cend()) {
            auto option = std::find_if(options.cbegin(), options.cend(),
                                    [it](const pair<string, function<void()>> &pair) -> bool {
                                        return *it == pair.first;
                                    });
            if (option != options.cend()) {
                it++;
                option->second();
            } else if (it->find("!", 0) == 0) {
                it++; // used to ignore options (to easily enable/disable options in debug settings etc.)
            } else if (it->find("-", 0) == 0) {
                usage();
            } else {
                if (initial_seek_seconds != 0 && filesystem::is_fifo(*it)) {
                    cerr << "Initial seek is not compatible with reading from fifo" << endl;
                    exit(EXIT_FAILURE);
                }
                if (!filesystem::exists(*it)) {
                    cerr << "File not found: " << *it << endl;
                    exit(EXIT_FAILURE);
                }

                InputFormat input_format;
                if (input_format_option.has_value())
                    input_format = input_format_option.value();
                else if (auto detected = inputFormatFromFilename(*it); detected.has_value())
                    input_format = detected.value();
                else {
                    cerr << "No input format specified and unknown input file extension" << endl;
                    exit(EXIT_FAILURE);
                }

                StreamLogger log(log_selection, std::cerr, true);

                musevk::VulkanManager manager(log);

                switch (input_type) {
                    case eNtscRf: {
                        auto *reader = new NtscFrameReader(
                                        log, executable_dir, manager, *it, input_format,
                                        input_sample_frequency, initial_seek_seconds, benchmark_shaders,
                                        muse_output_filename);
                        process_file<NtscInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                                     full_screen, no_sync, start_paused, decode_video, dropout_mode, decode_audio,
                                                     efm_audio,
                                                     benchmark_shaders, output_filename);
                        delete reader;
                        break;
                    }
                    case eMuse16MHz: {
                        auto *reader = new PhaseCorrect16MHzFrameReader(
                                log, *it, input_format, initial_seek_seconds, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, output_filename);
                        delete reader;
                        break;
                    }
                    case eMuseOversampled:
                    case eMuseRf: {
                        auto *reader = new ResamplingFrameReader(
                                log, executable_dir, manager, *it, input_format,
                                input_sample_frequency, initial_seek_seconds, input_type == eMuseRf, benchmark_shaders,
                                efm_audio, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, output_filename);
                        delete reader;
                        break;
                    }
                    default:
                        throw std::runtime_error("Unknown input type: {}");
                }
                it++;
            }
        }
    } catch (const exception &x) {
        StreamLogger log(StreamLogger::c_log_all, std::cerr, true);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
