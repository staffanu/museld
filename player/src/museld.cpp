// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <chrono>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

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
#include "CliOptions.h"

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

// The subtitle files available for one input file; the [ and ] keys cycle the
// primary (bottom) and secondary (top) display slots through these.
struct SubtitleSetup {
    std::vector<std::pair<std::string, std::string>> files; // {OSD label, path}, alphabetical
    int primary_index = -1; // initial primary track
    std::optional<std::string> font_path;
};

// Find the .srt files next to the input whose names start with the input's own
// name minus its extension: capture.raw matches capture.srt, capture.ja.srt,
// capture.ja-hira.srt, ...  The label is what follows the shared stem ("ja"
// for capture.ja.srt).
static std::vector<std::pair<std::string, std::string>> discoverSubtitleFiles(
        const std::filesystem::path &input_path) {
    std::vector<std::pair<std::string, std::string>> found;
    const std::string stem = input_path.stem().string();
    std::filesystem::path dir = input_path.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".srt") != 0) continue;
        if (name.compare(0, stem.size(), stem) != 0) continue;
        std::string label = name.substr(stem.size(), name.size() - stem.size() - 4);
        while (!label.empty() && label.front() == '.') label.erase(label.begin());
        if (label.empty()) label = "DEFAULT";
        found.emplace_back(std::move(label), entry.path().string());
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });
    return found;
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
                      bool initial_film_mode,
                      DropoutMode dropout_mode,
                      bool efm_audio,
                      bool benchmark_shaders,
                      bool output_yuv,
                      const std::unique_ptr<VideoFileWriter> &vfw,
                      AudioPlayback *audio_playback,
                      const std::string &executable_dir,
                      const SubtitleSetup &subtitle_setup,
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
        state.film_mode = initial_film_mode;

        OsdOverlay osd;
        FrameBlitter blitter;
        InputController input(log);
        FrameExporter frame_exporter(log, manager, command_pool);

        // All tracks are parsed and warmed into the shared glyph atlas up front,
        // so the [ and ] keys can switch tracks without any loading hitch.
        std::unique_ptr<SubtitleOverlay> subtitle_primary_overlay;
        std::unique_ptr<SubtitleOverlay> subtitle_secondary_overlay;
        if (!subtitle_setup.files.empty()) {
            try {
                auto tracks = std::make_shared<std::vector<SubtitleTrack>>();
                for (const auto &[label, path] : subtitle_setup.files) {
                    auto entries = parseSrt(path);
                    log.info(eApplication, std::format("Loaded {} subtitle entries from {} [{}]",
                                                       entries.size(), path, label));
                    tracks->push_back({label, std::move(entries)});
                }
                std::filesystem::path font_path = subtitle_setup.font_path
                    ? std::filesystem::path(*subtitle_setup.font_path)
                    : std::filesystem::path(executable_dir) / "fonts" / "NotoSansJP-Regular.ttf";
                const int frame_h = (int)decoder.getResultImages().out_image->getHeight();
                const int pixel_height = std::max(20, frame_h / 18);
                auto font = std::make_shared<SubtitleFont>(font_path, pixel_height, manager, command_pool);
                for (const auto &track : *tracks)
                    for (const auto &e : track.entries)
                        for (const auto &line : e.lines)
                            font->warmUpLine(utf8ToCodepoints(line));
                font->finalizeAtlas(command_pool);
                subtitle_primary_overlay = std::make_unique<SubtitleOverlay>(
                    tracks, font, false, executable_dir, manager, command_pool);
                subtitle_secondary_overlay = std::make_unique<SubtitleOverlay>(
                    tracks, font, true, executable_dir, manager, command_pool);
                for (const auto &[label, path] : subtitle_setup.files)
                    state.subtitle_track_names.push_back(label);
                state.subtitle_primary = subtitle_setup.primary_index;
            } catch (const std::exception &x) {
                log.error(eApplication, std::format("Subtitles disabled: {}", x.what()));
                subtitle_primary_overlay.reset();
                subtitle_secondary_overlay.reset();
                state.subtitle_track_names.clear();
                state.subtitle_primary = state.subtitle_secondary = -1;
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
                    state.film_mode,
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
            if (subtitle_primary_overlay)
                subtitle_primary_overlay->render(*command_buffer, images, state, decoder,
                                                 state.subtitle_primary);
            if (subtitle_secondary_overlay)
                subtitle_secondary_overlay->render(*command_buffer, images, state, decoder,
                                                   state.subtitle_secondary);
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
                  bool use_3d_comb, bool film_mode, bool decode_video, DropoutMode dropout_mode,
                  bool decode_audio, bool efm_audio, bool benchmark_shaders,
                  MuseAdaptiveEqualizer::Mode eq_mode, float eq_alpha,
                  float tint_degrees, float saturation,
                  optional<string> const &output_filename,
                  [[maybe_unused]] VideoWriterPreset write_preset,
                  const SubtitleSetup &subtitle_setup,
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
                  field_interpolation_mode, use_3d_comb, film_mode, dropout_mode, efm_audio, benchmark_shaders,
                  output_filename.has_value(),
                  vfw, audio_playback.get(), executable_dir,
                  subtitle_setup,
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

// The directory holding shaders/ and fonts/, i.e. the one containing the running
// executable. argv[0] alone is not enough: it has no directory part when the
// binary is invoked by bare name (through PATH, or in cmd.exe from the current
// directory), so ask the OS and keep argv[0] only as a last resort.
static string get_executable_dir(const char *argv0) {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    while (buffer.size() <= 65536) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
        if (length == 0)
            break;
        if (length < buffer.size())
            return std::filesystem::path(buffer.data()).parent_path().string();
        buffer.resize(2 * buffer.size()); // truncated
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::error_code ec;
        auto path = std::filesystem::weakly_canonical(buffer.data(), ec);
        if (!ec)
            return path.parent_path().string();
    }
#else
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return path.parent_path().string();
#endif
    return std::filesystem::path(argv0).parent_path().string();
}

int main(int argc, char *argv[]) {
    auto log_selection = StreamLogger::c_log_warn;
    std::string executable_dir = get_executable_dir(argv[0]);
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
    bool film_mode = true;
    optional<string> export_frame_filename; // save one frame as PNG and quit
    double export_frame_at_seconds = 0;     // stream position of the exported frame (like --seek)
    optional<string> muse_output_filename; // always written as little endian unsigned short values
    optional<string> output_filename; // container/codec selected by --write-preset
    VideoWriterPreset write_preset = VideoWriterPreset::eStandard;
    // "Run to the end" is a finite sentinel: infinity does not exist under -ffast-math,
    // and comparing against it is undefined there (same trap as NaN sentinels).
    double write_duration_seconds = std::numeric_limits<double>::max();
    bool decode_video = true;
    DropoutMode dropout_mode = DropoutMode::eNormal;
    bool decode_audio = true;
    bool efm_audio = false;
    bool benchmark_shaders = false;
    MuseAdaptiveEqualizer::Mode eq_mode = MuseAdaptiveEqualizer::Mode::eAdapt;
    constexpr float eq_alpha = 0.005f;
    float tint_degrees = 0.0f;
    float saturation = 1.0f;
    bool subtitles_enabled = false;
    optional<string> subtitles_file;
    optional<string> subtitle_font_path;

    const vector<string> args(argv + 1, argv + argc);
    auto it = args.cbegin();

    CliOptions options;

    auto usage = [&options] (ostream &out, int status) -> void {
        options.printHelp(out, "museld [options] <input_file> ...");
        out << "\nSeveral input files can be given, with options in between; each one is played with\n"
               "the options in effect where it appears.  An argument starting with ! is ignored,\n"
               "which is practical for disabling an option in a saved command line.\n";
        exit(status);
    };

    options.section("Input options:");
    options.option("--input-format", "FMT",
                   "Input sample type: u8, s8, u16, s16, u16be, s16be, lds, flac, ldf "
                   "(default: from the filename extension)", [&] () -> void {
        input_format_option = inputFormatFromString(*(it++));
    });
    options.option("--input-type", "TYPE",
                   "muse-rf (default) or ntsc-rf for RF captures, muse-16 for phase correct "
                   "16.2 MHz MUSE baseband, muse-os for oversampled MUSE baseband", [&] () -> void {
        const auto &name = *(it++);
        if      (name == "muse-rf")  input_type = eMuseRf;
        else if (name == "ntsc-rf")  input_type = eNtscRf;
        else if (name == "muse-16")  input_type = eMuse16MHz;
        else if (name == "muse-os")  input_type = eMuseOversampled;
        else throw std::runtime_error(std::format("Unknown input type {}", name));
    });
    options.option("--sample-freq", "HZ",
                   "Input sample rate, written as 62.5e6 rather than 62.5 (default 62.5e6, "
                   "the MUSE RF rate; NTSC RF captures are usually 40e6)", [&] () -> void {
        input_sample_frequency = stod(*(it++));
    });
    options.option("--seek", "SECONDS", "Seek to this position before playing", [&] () -> void {
        initial_seek_seconds = stod(*(it++));
    });

    options.section("Playback options:");
    options.flag("--full-screen", "Start full screen", [&] () -> void {
        full_screen = true;
    });
    options.flag("--pause", "Start paused", [&] () -> void {
        start_paused = true;
    });
    options.flag("--no-video", "Do not decode video", [&] () -> void {
        decode_video = false;
    });
    options.flag("--no-audio", "Do not decode audio", [&] () -> void {
        decode_audio = false;
    });
    options.flag("--efm", "Take the audio from the EFM (CD) track instead of the MUSE audio "
                          "(RF input only)", [&] () -> void {
        efm_audio = true;
    });
    options.flag("--all-fields", "Update the display once per field, at 60 Hz (default)", [&] () -> void {
        decode_all_fields = true;
    });
    options.flag("--full-frames-only", "Update the display once per frame, which halves the CPU "
                                       "and GPU load", [&] () -> void {
        decode_all_fields = false;
    });
    options.option("--field-interpolation", "MODE",
                   "Initial de-interlacing, as keys 1/2/3: normal (motion adaptive, default), "
                   "intra-field, inter-frame", [&] () -> void {
        const auto &name = *(it++);
        if      (name == "normal")      field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        else if (name == "intra-field") field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
        else if (name == "inter-frame") field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
        else throw std::runtime_error(std::format("Unknown --field-interpolation {} (expected normal|intra-field|inter-frame)", name));
    });
    options.flag("--no-3d-comb", "NTSC: start with the temporal Y/C separation off, as key 4", [&] () -> void {
        use_3d_comb = false;
    });
    options.flag("--no-film-mode", "NTSC: start with the film mode off instead of auto, as key 5 "
                                   "(auto weaves fields by the film frame they came from while "
                                   "a 3:2 pulldown cadence is locked)", [&] () -> void {
        film_mode = false;
    });
    options.flag("--no-dropout", "Leave detected dropouts untouched instead of concealing them", [&] () -> void {
        dropout_mode = DropoutMode::eDisabled;
    });
    options.flag("--highlight-dropout", "Paint dropouts red (luminance) or green (color) instead "
                                        "of concealing them", [&] () -> void {
        dropout_mode = DropoutMode::eHighlight;
    });
    options.option("--eq", "MODE",
                   "MUSE adaptive equalizer: on (default, taps adapt continuously), frozen "
                   "(keep the current taps), off (bypass)", [&] () -> void {
        const auto &name = *(it++);
        if      (name == "off")    eq_mode = MuseAdaptiveEqualizer::Mode::eOff;
        else if (name == "on")     eq_mode = MuseAdaptiveEqualizer::Mode::eAdapt;
        else if (name == "frozen") eq_mode = MuseAdaptiveEqualizer::Mode::eFrozen;
        else throw std::runtime_error(std::format("Unknown --eq mode {} (expected off|on|frozen)", name));
    });
    options.option("--tint", "DEGREES", "NTSC: rotate the chroma hue, like a TV's tint control "
                                        "(default 0)", [&] () -> void {
        tint_degrees = (float)stod(*(it++));
    });
    options.option("--saturation", "FACTOR", "NTSC: scale the chroma gain (default 1.0)", [&] () -> void {
        saturation = (float)stod(*(it++));
    });
    options.flag("--subtitles", "Display SRT subtitles synced to the disc's own time code.  The "
                                "tracks are the .srt files next to the input file whose names "
                                "start with the input's name minus its extension "
                                "(capture.ja.srt for capture.raw); the first one alphabetically "
                                "starts as the primary track, and the [ and ] keys cycle the "
                                "primary and secondary track", [&] () -> void {
        subtitles_enabled = true;
    });
    options.option("--subtitles-file", "FILE", "Start with the SRT subtitles in FILE as the "
                                               "primary track (implies --subtitles)", [&] () -> void {
        subtitles_file = *(it++);
        if (!filesystem::exists(*subtitles_file)) {
            cerr << "Subtitle file not found: " << *subtitles_file << endl;
            exit(EXIT_FAILURE);
        }
    });
    options.option("--subtitle-font", "FILE", "TrueType font for the subtitles (default: the "
                                              "bundled Noto Sans JP)", [&] () -> void {
        subtitle_font_path = *(it++);
        if (!filesystem::exists(*subtitle_font_path)) {
            cerr << "Subtitle font not found: " << *subtitle_font_path << endl;
            exit(EXIT_FAILURE);
        }
    });
    options.flag("--no-sync", "Decode as fast as the machine allows instead of at playback speed",
                 [&] () -> void {
        no_sync = true;
    });

    options.section("Output options:");
    options.option("--export-frame", "FILE", "Save one decoded frame as PNG to FILE and quit", [&] () -> void {
        export_frame_filename = *(it++);
    });
    options.option("--export-frame-at", "SECONDS",
                   "Stream position of the frame saved by --export-frame (absolute, like --seek, "
                   "not a delay after it), which also gives the decoder a warm-up run "
                   "(default: the first decoded frame)", [&] () -> void {
        export_frame_at_seconds = stod(*(it++));
    });
    options.option("--write", "FILE", "Write the decoded video and audio to a media file "
                                      "(requires FFmpeg)", [&] () -> void {
#ifdef HAVE_LIBAV
        output_filename = *(it++);
#else
        throw std::runtime_error("FFMPEG is not available");
#endif
    });
    options.option("--write-preset", "PRESET",
                   "Media file preset: standard (H.264 + AAC in MP4, default) or archival "
                   "(lossless FFV1 16-bit + PCM in Matroska)", [&] () -> void {
        const auto &name = *(it++);
        if      (name == "standard") write_preset = VideoWriterPreset::eStandard;
        else if (name == "archival") write_preset = VideoWriterPreset::eArchival;
        else throw std::runtime_error(std::format("Unknown --write-preset {} (expected standard|archival)", name));
    });
    options.option("--write-duration", "SECONDS",
                   "Finalize the media file after this much video (default: run to the end of "
                   "the input)", [&] () -> void {
        write_duration_seconds = stod(*(it++));
    });
    options.option("--write-muse16", "FILE",
                   "Re-encode the input to the 16.2 MHz MUSE format that --input-type muse-16 "
                   "reads", [&] () -> void {
        muse_output_filename = *(it++);
    });

    options.section("Diagnostics:");
    options.option("--log", "SPEC",
                   "Log level per category, e.g. A3V4.  Categories are MPAVDIO (Main, "
                   "Performance, Audio, Video, Decoder, Input, Output) and levels 0-4 "
                   "(off, error, warn, info, debug; default 2)", [&] () -> void {
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
    options.flag("--benchmark-shaders", "Print GPU shader timing statistics when playback ends",
                 [&] () -> void {
        benchmark_shaders = true;
    });
    options.flag("--help", "This text", [&] () -> void {
        usage(cout, EXIT_SUCCESS);
    });

    try {
        bool input_file_given = false;
        while (it != args.cend()) {
            if (const CliOptions::Option *option = options.find(*it)) {
                it++;
                if (option->takesArgument() && it == args.cend())
                    throw runtime_error(std::format("{} needs an argument ({})", option->name, option->argument));
                option->action();
            } else if (it->find("!", 0) == 0) {
                it++; // used to ignore options (to easily enable/disable options in CLion debug settings)
            } else if (it->find("-", 0) == 0) {
                cerr << "Unknown option: " << *it << endl;
                usage(cerr, EXIT_FAILURE);
            } else {
                input_file_given = true;
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

                SubtitleSetup subtitle_setup;
                subtitle_setup.font_path = subtitle_font_path;
                if (subtitles_enabled)
                    subtitle_setup.files = discoverSubtitleFiles(*it);
                if (subtitles_file) {
                    // If the discovery also found the file, keep the discovered entry
                    // (with its short label); otherwise add it, labeled by filename.
                    std::error_code ec;
                    const auto canon = filesystem::weakly_canonical(*subtitles_file, ec);
                    for (size_t i = 0; i < subtitle_setup.files.size(); i++)
                        if (filesystem::weakly_canonical(subtitle_setup.files[i].second, ec) == canon)
                            subtitle_setup.primary_index = (int)i;
                    if (subtitle_setup.primary_index < 0) {
                        subtitle_setup.files.emplace_back(
                                filesystem::path(*subtitles_file).stem().string(), *subtitles_file);
                        subtitle_setup.primary_index = (int)subtitle_setup.files.size() - 1;
                    }
                } else if (!subtitle_setup.files.empty()) {
                    subtitle_setup.primary_index = 0;
                }
                if (subtitles_enabled && subtitle_setup.files.empty())
                    cerr << "No .srt files matching " << filesystem::path(*it).stem().string()
                         << "*.srt found next to " << *it << endl;

                StreamLogger log(log_selection, std::cerr, true);

                musevk::VulkanManager manager(log);

                const double export_frame_after_seconds = max(0.0, export_frame_at_seconds - initial_seek_seconds);
                if (export_frame_filename && initial_seek_seconds > 0 && export_frame_at_seconds <= initial_seek_seconds)
                    cerr << std::format(
                            "Warning: --export-frame will save the first decoded frame, which comes out black "
                            "(the decoder has not locked yet). --export-frame-at takes the absolute stream "
                            "position of the frame to save -- not a delay after --seek -- and must lie beyond "
                            "the seek position ({} s) to give the decoder a warm-up run.",
                            initial_seek_seconds) << endl;

                switch (input_type) {
                    case eNtscRf: {
                        auto *reader = new NtscFrameReader(
                                        log, executable_dir, manager, *it, input_format,
                                        input_sample_frequency, initial_seek_seconds, benchmark_shaders, efm_audio,
                                        muse_output_filename);
                        process_file<NtscInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                                     efm_audio,
                                                     benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds);
                        delete reader;
                        break;
                    }
                    case eMuse16MHz: {
                        auto *reader = new PhaseCorrect16MHzFrameReader(
                                log, *it, input_format, initial_seek_seconds, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
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
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
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
        // Nothing to play is not an error, but say so: an empty command line
        // otherwise looks just like the player failing to start
        if (!input_file_given) {
            cerr << "No input file given -- museld plays a capture file, so it needs one to play." << endl;
            cerr << "Run \"museld --help\" for the options, or give a filename:  museld capture.lds" << endl;
        }
    } catch (const exception &x) {
        StreamLogger log(StreamLogger::c_log_all, std::cerr, true);
        log.error(eApplication, x.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
