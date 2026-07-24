// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <filesystem>
#include <format>
#include <functional>
#include <chrono>
#include "musevk/TimestampQueryPool.h"
#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"
#include "logging/StreamLogger.h"
#include "AudioPlayback.h"
#include "FrameReader.h"
#include "TextRenderer.h"
#include "Decoder.h"
#include "PlayerState.h"
#include "OsdOverlay.h"
#include "FrameBlitter.h"
#include "FrameExporter.h"
#include "InputController.h"
#include "subtitles/SrtParser.h"
#include "subtitles/SubtitleFont.h"
#include "subtitles/SubtitleOverlay.h"
#include "input/InputReader.h"
#include "input/InputReaderFactory.h"
#include "muse/ResamplingFrameReader.h"
#include "muse/PhaseCorrect16MHzFrameReader.h"
#include "muse/MuseDecoder.h"
#include "muse/MuseConstants.h"
#include "ntsc/NtscConstants.h"
#include "ntsc/NtscFrameReader.h"
#include "ntsc/NtscDecoder.h"
#include "VideoWriterOptions.h"

#ifdef HAVE_LIBAV
# include "VideoFileWriter.h"
#else
class VideoFileWriter {}; // stub for non-libav builds; never instantiated
#endif

#define INPUT_BUFFER_COUNT 6

using namespace std;

void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "Error %d: %s\n", error, description); // FIXME: use logging framework
}

static void runPlayer(Logger &log,
                      musevk::VulkanManager &manager,
                      Decoder &decoder,
                      ReaderControls &reader_controls,
                      GLFWwindow *window,
                      bool full_screen,
                      bool start_paused,
                      Decoder::FieldInterpolationMode initial_field_interpolation_mode,
                      bool initial_use_3d_comb,
                      DropoutMode dropout_mode,
                      bool efm_audio,
                      bool benchmark_shaders,
                      bool output_yuv,
                      const std::unique_ptr<VideoFileWriter> &vfw,
                      AudioPlayback *audio_playback,
                      const std::string &executable_dir,
                      const std::optional<std::string> &subtitles_path,
                      const std::optional<std::string> &subtitle_font_path,
                      const std::optional<std::string> &export_frame_filename,
                      double export_frame_after_seconds,
                      double write_duration_seconds,
                      double seconds_per_iteration) {
    vk::Device &device = manager.getDevice();

    vk::SemaphoreCreateInfo semaphoreInfo{};
    auto image_available_semaphore = device.createSemaphore(semaphoreInfo);
    auto render_finished_semaphore = device.createSemaphore(semaphoreInfo);

    {
        musevk::CommandPool command_pool(manager);
        auto command_buffer = command_pool.createCommandBuffer();
        TextRenderer text_renderer(executable_dir, manager, command_pool);

        auto images = decoder.getResultImages();
        const auto src_dims = decoder.getSourceDimensions();

        PlayerState state;
        state.paused_countdown = start_paused ? 5 : 0;
        state.field_interpolation_mode = initial_field_interpolation_mode;
        state.use_3d_comb = initial_use_3d_comb;

        OsdOverlay osd;
        FrameBlitter blitter;
        InputController input(log);
        FrameExporter frame_exporter(log, manager, command_pool);

        std::unique_ptr<SubtitleOverlay> subtitle_overlay;
        if (subtitles_path) {
            try {
                auto entries = parseSrt(*subtitles_path);
                log.info(eApplication, std::format("Loaded {} subtitle entries from {}", entries.size(), *subtitles_path));
                std::filesystem::path font_path = subtitle_font_path
                    ? std::filesystem::path(*subtitle_font_path)
                    : std::filesystem::path(executable_dir) / "fonts" / "NotoSansJP-Regular.ttf";
                const int frame_h = (int)decoder.getResultImages().out_image->getHeight();
                const int pixel_height = std::max(20, frame_h / 18);
                auto font = std::make_unique<SubtitleFont>(font_path, pixel_height, manager, command_pool);
                for (const auto &e : entries)
                    for (const auto &line : e.lines)
                        font->warmUpLine(utf8ToCodepoints(line));
                font->finalizeAtlas(command_pool);
                subtitle_overlay = std::make_unique<SubtitleOverlay>(
                    std::move(entries), std::move(font), executable_dir, manager, command_pool);
            } catch (const std::exception &x) {
                log.error(eApplication, std::format("Subtitles disabled: {}", x.what()));
            }
        }

        auto t0 = chrono::high_resolution_clock::now();

        auto make_controls = [&](bool redo) {
            return Decoder::DecodeControls{
                    efm_audio,
                    state.field_interpolation_mode,
                    redo,
                    state.enable_non_linear,
                    state.use_3d_comb,
                    dropout_mode,
                    output_yuv,
            };
        };

        while ((state.paused && !state.redo_last_field)
               || decoder.next(make_controls(state.redo_last_field), state.last_decoded)) {

            // Only fields that were actually decoded: next() also returns true
            // when the input timed out, and counting those would make
            // --export-frame-at fire early and the frame rate below flattering.
            if (!state.paused && state.last_decoded.decoded)
                state.field_count++;
            state.redo_last_field = false;

            // Scripted export: decode until the requested stream position is reached
            bool scripted_export = false;
            if (export_frame_filename &&
                state.field_count * seconds_per_iteration >= export_frame_after_seconds) {
                state.export_frame = true;
                scripted_export = true;
            }

            // Export before the OSD and subtitles are drawn into the image, so
            // the file contains only the decoded picture
            if (state.export_frame) {
                state.export_frame = false;
                auto path = frame_exporter.exportFrame(*images.out_image,
                                                       scripted_export ? export_frame_filename : std::nullopt);
                if (scripted_export)
                    break;
                state.osd_text = path ? std::format("SAVED {}", std::filesystem::path(*path).filename().string())
                                      : "EXPORT FAILED";
            }

#ifdef HAVE_LIBAV
            if (vfw) {
                vfw->addVideoFrameWithAudio(images.out_Y, images.out_U, images.out_V,
                                            state.last_decoded.audio_mode,
                                            state.last_decoded.audio_sample_count,
                                            state.last_decoded.audio_samples);
                // Breaking out finalizes the file in the writer's destructor
                if (state.field_count * seconds_per_iteration >= write_duration_seconds)
                    break;
            }
#endif

            if (audio_playback && state.last_decoded.audio_sample_count != 0
                && state.last_decoded.audio_mode != MODE_UNKNOWN && !state.paused) {
                audio_playback->add_samples(state.last_decoded.audio_mode,
                                            state.last_decoded.audio_sample_count,
                                            state.last_decoded.audio_samples);
            }

            auto swap_chain_image = manager.acquireNextImage(image_available_semaphore);

            command_buffer->begin();
            state.last_cursor_string = osd.render(*command_buffer, images, state, decoder, window, text_renderer);
            if (subtitle_overlay)
                subtitle_overlay->render(*command_buffer, images, state, decoder);
            blitter.present(*command_buffer, images, state, src_dims,
                            swap_chain_image, manager.getSwapChainExtent(), manager,
                            image_available_semaphore);

            manager.present(swap_chain_image);

            if (state.paused_countdown != 0 && --state.paused_countdown == 0) {
                state.paused = true;
                state.osd_text = "PAUSE";
            }
            if (glfwWindowShouldClose(window))
                break;
            if (!input.poll(window, state, reader_controls, dropout_mode, efm_audio,
                            full_screen, src_dims.width, src_dims.height))
                break;
        }

        auto t1 = chrono::high_resolution_clock::now();
        auto time_us = (double) chrono::duration_cast<chrono::microseconds>(t1 - t0).count();
        if (state.field_count > 0) {
            log.info(eApplication | ePerformance,
                std::format("Total {} frames.  Avg {:.3f} ms/frame ({:.3f} frames/s)",
                            state.field_count / 2,
                            time_us / 1000.0 / state.field_count * 2,
                            1000000.0 / time_us * state.field_count / 2));
        }
        if (benchmark_shaders)
            decoder.outputBenchmarkResults();
    }

    device.destroy(image_available_semaphore);
    device.destroy(render_finished_semaphore);
}

template<class InputBlock>
void process_file(Logger &log, const string &executable_dir, musevk::VulkanManager &manager, FrameReader<InputBlock> &reader,
                  bool decode_all_fields, bool full_screen, bool no_sync,
                  bool start_paused, Decoder::FieldInterpolationMode field_interpolation_mode,
                  bool use_3d_comb, bool decode_video, DropoutMode dropout_mode,
                  bool decode_audio, bool efm_audio, bool benchmark_shaders,
                  MuseAdaptiveEqualizer::Mode eq_mode, float eq_alpha,
                  float tint_degrees, float saturation,
                  optional<string> const &output_filename,
                  [[maybe_unused]] VideoWriterPreset write_preset,
                  optional<string> const &subtitles_path,
                  optional<string> const &subtitle_font_path,
                  optional<string> const &export_frame_filename,
                  double export_frame_after_seconds,
                  double write_duration_seconds) {
    glfwSetErrorCallback(glfw_error_callback);
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    int initial_w, initial_h;
    const char *title;
    if constexpr (std::is_same<InputBlock, MuseInputBlock>::value) {
        initial_w = MUSE_Y_BUF_WIDTH * 3;
        initial_h = MUSE_BUF_HEIGHT * 2;
        title = "MUSE";
    } else {
        initial_w = NTSC_Y_BUF_WIDTH;
        initial_h = NTSC_FIELD_HEIGHT * 2;
        title = "NTSC";
    }
    GLFWwindow *window = glfwCreateWindow(initial_w, initial_h, title,
                                          full_screen ? glfwGetPrimaryMonitor() : nullptr, nullptr);
    glfwSetInputMode(window, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

    manager.initVulkan(window, no_sync);

    {
        std::vector<std::unique_ptr<InputBlock>> input_vulkan_buffers{};
        for (int i = 0; i < INPUT_BUFFER_COUNT; i++)
            input_vulkan_buffers.push_back(InputBlockFactory<InputBlock>::makeBlock(manager));
        if (!reader.initialize(input_vulkan_buffers))
            throw runtime_error("FrameReader initialization failed");
    }

    unique_ptr<VideoFileWriter> vfw;
#ifdef HAVE_LIBAV
    if (output_filename) {
        VideoColorStandard color_standard;
        int dar_num, dar_den; // display aspect ratio
        int fps_num, fps_den;
        if constexpr (std::is_same<InputBlock, MuseInputBlock>::value) {
            color_standard = VideoColorStandard::eBt709;
            dar_num = 16; dar_den = 9;
            fps_num = decode_all_fields ? 60 : 30; fps_den = 1;
        } else {
            color_standard = VideoColorStandard::eSmpte170m;
            dar_num = 4; dar_den = 3;
            fps_num = decode_all_fields ? 60000 : 30000; fps_den = 1001;
        }
        vfw = make_unique<VideoFileWriter>(output_filename.value(), log,
                                           initial_w, initial_h, fps_num, fps_den,
                                           write_preset, color_standard, dar_num, dar_den);
        if (!vfw->init())
            throw runtime_error("Cannot initialize output encoder");
    }
#endif

    unique_ptr<AudioPlayback> audio_playback = nullptr;
    if (decode_audio)
        audio_playback = make_unique<AudioPlayback>(log);

    {
        musevk::CommandPool command_pool(manager);

        std::unique_ptr<musevk::TimestampQueryPool> timestamp_query_pool = benchmark_shaders
                ? std::make_unique<musevk::TimestampQueryPool>(manager.getPhysicalDevice(), manager.getDevice(), 40)
                : nullptr;

        ReaderControls reader_controls{
                [&reader](double seconds) { reader.seek(seconds); },
                [&reader](bool enabled) { reader.setEfmEnabled(enabled); },
        };

        std::unique_ptr<Decoder> decoder;
        if constexpr (std::is_same<InputBlock, MuseInputBlock>::value) {
            auto mp = std::make_unique<MuseDecoder>(log, (FrameReader<MuseInputBlock> &)reader,
                                                    manager, command_pool, executable_dir,
                                                    decode_video, decode_all_fields, decode_audio,
                                                    eq_mode, eq_alpha,
                                                    timestamp_query_pool.get());
            // This isn't a great use of unique_ptr
            MuseDecoder *muse_decoder = mp.get();
            reader_controls.cycleEqMode = [muse_decoder]() -> std::string {
                switch (muse_decoder->cycleEqMode()) {
                    case MuseAdaptiveEqualizer::Mode::eOff:    return "OFF";
                    case MuseAdaptiveEqualizer::Mode::eAdapt:  return "ADAPT";
                    case MuseAdaptiveEqualizer::Mode::eFrozen: return "FROZEN";
                }
                return "?";
            };
            reader_controls.resetEqTaps = [muse_decoder]() { muse_decoder->resetEqTaps(); };
            decoder = std::move(mp);
        } else {
            decoder = std::make_unique<NtscDecoder>(log, (FrameReader<NtscInputBlock> &)reader,
                                                    manager, command_pool, executable_dir,
                                                    decode_video, decode_all_fields, decode_audio,
                                                    tint_degrees, saturation,
                                                    timestamp_query_pool.get());
        }

        if (!decoder->initialize())
            throw runtime_error("Decoder initialization failed");

        const double fields_per_second = std::is_same<InputBlock, MuseInputBlock>::value ? 60.0 : 60000.0 / 1001.0;
        const double seconds_per_iteration = (decode_all_fields ? 1 : 2) / fields_per_second;

        runPlayer(log, manager, *decoder, reader_controls, window, full_screen, start_paused,
                  field_interpolation_mode, use_3d_comb, dropout_mode, efm_audio, benchmark_shaders,
                  output_filename.has_value(),
                  vfw, audio_playback.get(), executable_dir,
                  subtitles_path, subtitle_font_path,
                  export_frame_filename, export_frame_after_seconds, write_duration_seconds, seconds_per_iteration);
    }

#ifdef HAVE_LIBAV
    if (vfw) {
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
    std::string executable_dir = std::filesystem::path(argv[0]).parent_path().string();
    bool decode_all_fields = true;
    bool full_screen = false;
    bool no_sync = false;
    InputType input_type = eMuseRf;
    std::optional<InputFormat> input_format_option = std::nullopt;
    double input_sample_frequency = 62.5e6;
    double initial_seek_seconds = 0;
    bool start_paused = false;
    auto field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
    bool use_3d_comb = true;
    optional<string> export_frame_filename; // save one frame as PNG and quit
    double export_frame_at_seconds = 0;     // stream position of the exported frame (like --seek)
    optional<string> muse_output_filename; // always written as little endian unsigned short values
    optional<string> output_filename; // container/codec selected by --write-preset
    VideoWriterPreset write_preset = VideoWriterPreset::eStandard;
    double write_duration_seconds = std::numeric_limits<double>::infinity();
    bool decode_video = true;
    DropoutMode dropout_mode = DropoutMode::eNormal;
    bool decode_audio = true;
    bool efm_audio = false;
    bool benchmark_shaders = false;
    MuseAdaptiveEqualizer::Mode eq_mode = MuseAdaptiveEqualizer::Mode::eAdapt;
    constexpr float eq_alpha = 0.005f;
    float tint_degrees = 0.0f;
    float saturation = 1.0f;
    optional<string> subtitles_path;
    optional<string> subtitle_font_path;

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
    options.emplace_back("--field-interpolation", [&] () mutable -> void {
        const auto &name = *(it++);
        if      (name == "normal")      field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        else if (name == "intra-field") field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
        else if (name == "inter-frame") field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
        else throw std::runtime_error(std::format("Unknown --field-interpolation {} (expected normal|intra-field|inter-frame)", name));
    });
    options.emplace_back("--no-3d-comb", [&] () mutable -> void {
        use_3d_comb = false;
    });
    options.emplace_back("--pause", [&] () mutable -> void {
        start_paused = true;
    });
    options.emplace_back("--export-frame", [&] () mutable -> void {
        export_frame_filename = *(it++);
    });
    options.emplace_back("--export-frame-at", [&] () mutable -> void {
        export_frame_at_seconds = stod(*(it++));
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
    options.emplace_back("--write-preset", [&] () mutable -> void {
        const auto &name = *(it++);
        if      (name == "standard") write_preset = VideoWriterPreset::eStandard;
        else if (name == "archival") write_preset = VideoWriterPreset::eArchival;
        else throw std::runtime_error(std::format("Unknown --write-preset {} (expected standard|archival)", name));
    });
    options.emplace_back("--write-duration", [&] () mutable -> void {
        write_duration_seconds = stod(*(it++));
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
                case 'M': log_selection[eApplication] = parseLevel((*it)[i + 1]); break;
                case 'P': log_selection[ePerformance] = parseLevel((*it)[i + 1]); break;
                case 'A': log_selection[eAudio] = parseLevel((*it)[i + 1]); break;
                case 'V': log_selection[eVideo] = parseLevel((*it)[i + 1]); break;
                case 'D': log_selection[eDecoder] = parseLevel((*it)[i + 1]); break;
                case 'I': log_selection[eInput] = parseLevel((*it)[i + 1]); break;
                case 'O': log_selection[eOutput] = parseLevel((*it)[i + 1]); break;
                default: throw runtime_error("Unknown log category");
            }
        }
        it++;
    });
    options.emplace_back("--benchmark-shaders", [&] () mutable -> void {
        benchmark_shaders = true;
    });
    options.emplace_back("--eq", [&] () mutable -> void {
        const auto &name = *(it++);
        if      (name == "off")    eq_mode = MuseAdaptiveEqualizer::Mode::eOff;
        else if (name == "on")     eq_mode = MuseAdaptiveEqualizer::Mode::eAdapt;
        else if (name == "frozen") eq_mode = MuseAdaptiveEqualizer::Mode::eFrozen;
        else throw std::runtime_error(std::format("Unknown --eq mode {} (expected off|on|frozen)", name));
    });
    options.emplace_back("--tint", [&] () mutable -> void {
        tint_degrees = (float)stod(*(it++));
    });
    options.emplace_back("--saturation", [&] () mutable -> void {
        saturation = (float)stod(*(it++));
    });
    options.emplace_back("--no-sync", [&] () mutable -> void {
        no_sync = true;
    });
    options.emplace_back("--subtitles", [&] () mutable -> void {
        subtitles_path = *(it++);
        if (!filesystem::exists(*subtitles_path)) {
            cerr << "Subtitle file not found: " << *subtitles_path << endl;
            exit(EXIT_FAILURE);
        }
    });
    options.emplace_back("--subtitle-font", [&] () mutable -> void {
        subtitle_font_path = *(it++);
        if (!filesystem::exists(*subtitle_font_path)) {
            cerr << "Subtitle font not found: " << *subtitle_font_path << endl;
            exit(EXIT_FAILURE);
        }
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
                it++; // used to ignore options (to easily enable/disable options in CLion debug settings)
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

                const double export_frame_after_seconds = max(0.0, export_frame_at_seconds - initial_seek_seconds);

                switch (input_type) {
                    case eNtscRf: {
                        auto *reader = new NtscFrameReader(
                                        log, executable_dir, manager, *it, input_format,
                                        input_sample_frequency, initial_seek_seconds, benchmark_shaders, efm_audio,
                                        muse_output_filename);
                        process_file<NtscInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, decode_video, dropout_mode, decode_audio,
                                                     efm_audio,
                                                     benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitles_path, subtitle_font_path,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds);
                        delete reader;
                        break;
                    }
                    case eMuse16MHz: {
                        auto *reader = new PhaseCorrect16MHzFrameReader(
                                log, *it, input_format, initial_seek_seconds, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitles_path, subtitle_font_path,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds);
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
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitles_path, subtitle_font_path,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds);
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
