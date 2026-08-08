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
#ifdef HAVE_OCR
# include <map>
# include "ocr/OcrBandCapture.h"
# include "ocr/OcrWorker.h"
# include "ocr/TranslationWorker.h"
#endif
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
    double offset_seconds = 0.0; // delays the subtitles (negative shows them earlier)
    // Directory holding the PP-OCR detection and recognition .onnx models;
    // set iff live subtitle OCR is enabled (--ocr)
    std::optional<std::string> ocr_models_dir;
    // OpenAI-compatible server translating the OCR track into a live "OCR-EN"
    // track; set iff --ocr-translate is given
    std::optional<std::string> ocr_translate_url;
    std::string ocr_translate_model; // empty: first model from /v1/models
    std::string ocr_translate_key;   // empty: no Authorization header
    std::string ocr_script = "cjk"; // cjk | latin | any: rows without it are dropped
    std::string ocr_source_language = "Japanese";
    std::string ocr_target_language = "English";
    // Non-empty: save the OCR (and OCR-EN) cues at exit as <stem>.OCR.srt /
    // <stem>.OCR-EN.srt with their original imprint timing (--ocr-write)
    std::string ocr_write_stem;
};

#ifdef HAVE_OCR
// The bottom fraction of the frame the OCR watches for burned-in subtitles
static constexpr double c_ocr_band_fraction = 0.28;

// Locates the detection and recognition models in the --ocr directory: the
// .onnx files whose names contain "det" and "rec".
static std::pair<std::string, std::string> findOcrModels(const std::string &dir) {
    std::string det, rec;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".onnx") continue;
        const std::string name = entry.path().filename().string();
        if (name.find("det") != std::string::npos) det = entry.path().string();
        else if (name.find("rec") != std::string::npos) rec = entry.path().string();
    }
    return {det, rec};
}

// Applies OCR text updates to the live "OCR" subtitle track (render thread
// only).  An update opens a cue at its stream time and the next one closes it;
// near-identical cues split by a momentary detection dropout are bridged by
// reopening the previous entry, mirroring tools/subocr/subocr.py.
struct LiveSubtitleFeed {
    static constexpr double c_merge_gap_seconds = 0.7;
    static constexpr double c_open_end = 1e9;          // end of a cue still on screen
    static constexpr double c_max_end_extension = 2.5; // cap on the late-display compensation

    int open_entry = -1;
    double open_display_delay = 0.0; // how late the open cue appeared on screen
    std::vector<std::string> last_lines;
    double last_end = -1e9;
    // Cues with their original imprint timing, unaffected by the display-side
    // lateness compensation below; this is what --ocr-write saves
    std::vector<SubtitleEntry> file_entries;

    void apply(const OcrWorker::Update &update, double now, SubtitleTrack &track,
               SubtitleFont &font) {
        // After a backward seek, drop cues from the abandoned timeline; the
        // rewatched span is re-OCRed and re-appended, keeping entries sorted
        while (!track.entries.empty() && track.entries.back().start_seconds > update.seconds) {
            track.entries.pop_back();
            open_entry = -1;
        }
        while (!file_entries.empty() && file_entries.back().start_seconds > update.seconds)
            file_entries.pop_back();
        if (open_entry >= 0) {
            // OCR latency made the cue appear open_display_delay late on
            // screen; keep it up correspondingly longer so its display time is
            // not shortened.  A later cue takes over the screen the moment it
            // is appended, so the overlap is harmless.
            track.entries[open_entry].end_seconds =
                    update.seconds + std::min(open_display_delay, c_max_end_extension);
            last_lines = track.entries[open_entry].lines;
            last_end = update.seconds;
            file_entries.push_back({track.entries[open_entry].start_seconds, update.seconds,
                                    last_lines});
            open_entry = -1;
        }
        if (update.lines.empty()) return;
        if (!track.entries.empty() && update.lines == last_lines
            && update.seconds - last_end <= c_merge_gap_seconds) {
            open_entry = (int)track.entries.size() - 1; // flicker: reopen the previous cue
            track.entries[open_entry].end_seconds = c_open_end;
            if (!file_entries.empty()) file_entries.pop_back(); // re-closed with the full span
            return;
        }
        for (const auto &line : update.lines)
            font.warmUpLine(utf8ToCodepoints(line));
        track.entries.push_back({update.seconds, c_open_end, update.lines});
        open_entry = (int)track.entries.size() - 1;
        open_display_delay = std::max(0.0, now - update.seconds);
    }

    // Close the still-open cue into the file record at end of playback.
    void finish(double now, SubtitleTrack &track) {
        if (open_entry < 0) return;
        file_entries.push_back({track.entries[open_entry].start_seconds, now,
                                track.entries[open_entry].lines});
        open_entry = -1;
    }
};

// Correlates translated cues with their Japanese originals and applies them to
// the live "OCR-EN" track.  An English cue inherits its Japanese cue's span:
// the translation typically arrives well before the cue leaves the screen, and
// the entry's start lying slightly in the past makes it display immediately.
struct TranslatedSubtitleFeed {
    struct Span { double start; double end; }; // end < 0: cue still on screen

    struct OpenEn {
        int index;            // index of the open-ended EN entry
        double display_delay; // how late its translation appeared on screen
    };

    long next_id = 1;
    long open_id = 0;           // ja cue currently on screen
    std::map<long, Span> spans; // cues whose translation is pending or open
    std::map<long, OpenEn> open_en_entries;
    std::vector<SubtitleEntry> file_entries; // original imprint timing, for --ocr-write

    // Track the ja-side update; returns the id to submit for translation
    // (0 = nothing new to translate).
    long onOcrUpdate(const OcrWorker::Update &update, SubtitleTrack &en_track) {
        // Backward seek: mirror LiveSubtitleFeed's truncation, and forget
        // pending cues from the abandoned timeline so late translations of
        // them are not applied
        if (!en_track.entries.empty() && en_track.entries.back().start_seconds > update.seconds) {
            while (!en_track.entries.empty()
                   && en_track.entries.back().start_seconds > update.seconds)
                en_track.entries.pop_back();
            std::erase_if(open_en_entries, [&](const auto &kv) {
                return kv.second.index >= (int)en_track.entries.size();
            });
            std::erase_if(spans, [&](const auto &kv) {
                return kv.second.start > update.seconds;
            });
            while (!file_entries.empty() && file_entries.back().start_seconds > update.seconds)
                file_entries.pop_back();
            if (open_id && !spans.count(open_id)) open_id = 0;
        }
        if (open_id) {
            if (auto it = open_en_entries.find(open_id); it != open_en_entries.end()) {
                // Same lateness compensation as the ja track: the translation
                // appeared display_delay late, so let it linger as long
                auto &entry = en_track.entries[it->second.index];
                entry.end_seconds = update.seconds
                        + std::min(it->second.display_delay, LiveSubtitleFeed::c_max_end_extension);
                file_entries.push_back({entry.start_seconds, update.seconds, entry.lines});
                open_en_entries.erase(it);
                spans.erase(open_id); // translated and closed: done
            } else {
                spans[open_id].end = update.seconds; // translation still pending
            }
            open_id = 0;
        }
        if (update.lines.empty()) return 0;
        const long id = next_id++;
        spans[id] = {update.seconds, -1.0};
        open_id = id;
        return id;
    }

    struct Applied {
        double start;        // the cue's start time
        double late_seconds; // > 0: cue had left the screen this long before
                             // its translation arrived (shown for a grace
                             // period from now instead)
    };

    // Applies one finished translation.  `now` is the current stream time: a
    // translation can arrive after its cue already left the screen (OCR +
    // translation latency exceeding the cue duration), and is then shown from
    // now for a bounded grace period instead of being appended entirely in the
    // past and never displaying.
    std::optional<Applied> onTranslation(const TranslationWorker::Result &result, double now,
                                         SubtitleTrack &en_track, SubtitleFont &font) {
        auto it = spans.find(result.id);
        if (it == spans.end() || result.lines.empty()) return std::nullopt;
        const Span span = it->second;
        // A backward seek may have replayed earlier cues since: keep sorted
        if (!en_track.entries.empty()
            && span.start < en_track.entries.back().start_seconds) {
            spans.erase(it);
            return std::nullopt;
        }
        double end = span.end < 0 ? LiveSubtitleFeed::c_open_end : span.end;
        double late_seconds = 0.0;
        if (span.end >= 0 && now >= span.end) {
            late_seconds = now - span.end;
            end = now + std::min(2.5, span.end - span.start);
        }
        for (const auto &line : result.lines)
            font.warmUpLine(utf8ToCodepoints(line));
        en_track.entries.push_back({span.start, end, result.lines});
        if (span.end < 0) {
            open_en_entries[result.id] = {(int)en_track.entries.size() - 1,
                                          std::max(0.0, now - span.start)};
        } else {
            file_entries.push_back({span.start, span.end, result.lines});
            spans.erase(it);
        }
        return Applied{span.start, late_seconds};
    }

    // Close still-open cues into the file record at end of playback.
    void finish(double now, SubtitleTrack &en_track) {
        for (const auto &[id, open] : open_en_entries)
            file_entries.push_back({en_track.entries[open.index].start_seconds, now,
                                    en_track.entries[open.index].lines});
        open_en_entries.clear();
    }
};
#endif

// "English" -> "EN": the short code naming the translated OCR track and its
// saved file ("OCR-EN", <input>.OCR-EN.srt).
static std::string languageCode(const std::string &language) {
    std::string code;
    for (char c : language) {
        if (code.size() == 2) break;
        if (isalpha((unsigned char)c)) code += (char)toupper((unsigned char)c);
    }
    return code.empty() ? "XX" : code;
}

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
                      double seconds_per_iteration,
                      double initial_seek_seconds) {
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
        state.stream_start_seconds = initial_seek_seconds;
        state.paused_countdown = start_paused ? 5 : 0;
        state.field_interpolation_mode = initial_field_interpolation_mode;
        state.use_3d_comb = initial_use_3d_comb;
        state.film_mode = initial_film_mode;

        OsdOverlay osd;
        FrameBlitter blitter;
        InputController input(log);
        FrameExporter frame_exporter(log, manager, command_pool);

        // All tracks are parsed and warmed into the shared glyph atlas up front,
        // so the [ and ] keys can switch tracks without any loading hitch.  The
        // OCR feature adds a live track to the same list; its entries (and
        // glyphs) arrive during playback, so the display path is shared between
        // file and live subtitles.
        std::unique_ptr<SubtitleOverlay> subtitle_primary_overlay;
        std::unique_ptr<SubtitleOverlay> subtitle_secondary_overlay;
        std::shared_ptr<std::vector<SubtitleTrack>> subtitle_tracks;
        std::shared_ptr<SubtitleFont> subtitle_font;
        int ocr_track_index = -1;
        int ocr_en_track_index = -1;
        if (!subtitle_setup.files.empty() || subtitle_setup.ocr_models_dir) {
            try {
                auto tracks = std::make_shared<std::vector<SubtitleTrack>>();
                for (const auto &[label, path] : subtitle_setup.files) {
                    auto entries = parseSrt(path);
                    log.info(eApplication, std::format("Loaded {} subtitle entries from {} [{}]",
                                                       entries.size(), path, label));
                    tracks->push_back({label, std::move(entries)});
                }
                if (subtitle_setup.ocr_models_dir) {
                    ocr_track_index = (int)tracks->size();
                    tracks->push_back({"OCR", {}});
                    if (subtitle_setup.ocr_translate_url) {
                        ocr_en_track_index = (int)tracks->size();
                        tracks->push_back({"OCR-" + languageCode(subtitle_setup.ocr_target_language), {}});
                    }
                }
                std::filesystem::path font_path = subtitle_setup.font_path
                    ? std::filesystem::path(*subtitle_setup.font_path)
                    : std::filesystem::path(executable_dir) / "fonts" / "NotoSansJP-Regular.ttf";
                const int frame_h = (int)decoder.getResultImages().out_image->getHeight();
                const int pixel_height = std::max(20, frame_h / 18);
                auto font = std::make_shared<SubtitleFont>(log, font_path, pixel_height, manager, command_pool);
                for (const auto &track : *tracks)
                    for (const auto &e : track.entries)
                        for (const auto &line : e.lines)
                            font->warmUpLine(utf8ToCodepoints(line));
                font->finalizeAtlas(command_pool);
                subtitle_primary_overlay = std::make_unique<SubtitleOverlay>(
                    log, tracks, font, false, subtitle_setup.offset_seconds, executable_dir, manager, command_pool);
                subtitle_secondary_overlay = std::make_unique<SubtitleOverlay>(
                    log, tracks, font, true, subtitle_setup.offset_seconds, executable_dir, manager, command_pool);
                for (const auto &track : *tracks)
                    state.subtitle_track_names.push_back(track.label);
                // With no file track chosen, the translated OCR track (when
                // enabled) beats the raw Japanese one as the default
                state.subtitle_primary = subtitle_setup.primary_index >= 0
                    ? subtitle_setup.primary_index
                    : (ocr_en_track_index >= 0 ? ocr_en_track_index : ocr_track_index);
                subtitle_tracks = std::move(tracks);
                subtitle_font = std::move(font);
            } catch (const std::exception &x) {
                log.error(eApplication, std::format("Subtitles disabled: {}", x.what()));
                subtitle_primary_overlay.reset();
                subtitle_secondary_overlay.reset();
                subtitle_tracks.reset();
                subtitle_font.reset();
                ocr_track_index = -1;
                ocr_en_track_index = -1;
                state.subtitle_track_names.clear();
                state.subtitle_primary = state.subtitle_secondary = -1;
            }
        }

#ifdef HAVE_OCR
        // The OCR worker is owned here, directly by the render thread; band
        // samples go out and text updates come back between frames, so all
        // track and glyph-atlas mutation happens on this thread.
        std::unique_ptr<OcrWorker> ocr_worker;
        std::unique_ptr<OcrBandCapture> ocr_band_capture;
        std::unique_ptr<TranslationWorker> ocr_translator;
        LiveSubtitleFeed ocr_feed;
        TranslatedSubtitleFeed ocr_translated_feed;
        int ocr_sample_countdown = 0;
        const int ocr_sample_every = std::max(1, (int)std::round(1.0 / (3.0 * seconds_per_iteration)));
        if (ocr_track_index >= 0) {
            auto [det_model, rec_model] = findOcrModels(*subtitle_setup.ocr_models_dir);
            if (det_model.empty() || rec_model.empty()) {
                log.error(eApplication, std::format("OCR disabled: no det/rec .onnx models in {}",
                                                    *subtitle_setup.ocr_models_dir));
                ocr_track_index = -1;
            } else {
                const OcrScriptFilter script_filter =
                        subtitle_setup.ocr_script == "latin" ? OcrScriptFilter::eLatin
                        : subtitle_setup.ocr_script == "any" ? OcrScriptFilter::eAny
                                                             : OcrScriptFilter::eCjk;
                ocr_worker = std::make_unique<OcrWorker>(log, det_model, rec_model, script_filter);
                ocr_band_capture = std::make_unique<OcrBandCapture>(manager, command_pool,
                                                                    c_ocr_band_fraction);
                if (ocr_en_track_index >= 0)
                    ocr_translator = std::make_unique<TranslationWorker>(
                            log, *subtitle_setup.ocr_translate_url,
                            subtitle_setup.ocr_translate_model, subtitle_setup.ocr_translate_key,
                            subtitle_setup.ocr_source_language,
                            subtitle_setup.ocr_target_language);
            }
        }
#endif

        auto t0 = chrono::high_resolution_clock::now();
        int disc_code_logged_minute = 0; // minute 0 is not logged: the decoder is still locking

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
            state.stream_seconds = state.field_count * seconds_per_iteration
                                   + state.stream_seek_offset_seconds;
            state.redo_last_field = false;

            // Once a minute of stream time, log the decoded field count against the
            // disc's own time code.  The offset is what to shift .srt files made from
            // a --write render by (whisper times are file-relative), and a changing
            // offset means playback and disc time are drifting apart.
            if (const double stream_seconds = state.field_count * seconds_per_iteration;
                static_cast<int>(stream_seconds / 60.0) != disc_code_logged_minute
                && state.last_decoded.disc_info) {
                disc_code_logged_minute = static_cast<int>(stream_seconds / 60.0);
                if (auto disc_seconds = state.last_decoded.disc_info->playbackTimeSeconds()) {
                    log.info(eApplication,
                             std::format("Stream {:.2f} s ({} fields), disc code {:.2f} s, offset {:+.2f} s",
                                         stream_seconds, state.field_count, *disc_seconds,
                                         *disc_seconds - stream_seconds));
                }
            }

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

#ifdef HAVE_OCR
            if (ocr_worker) {
                if (!state.paused && state.last_decoded.decoded
                    && ++ocr_sample_countdown >= ocr_sample_every) {
                    ocr_sample_countdown = 0;
                    int band_width, band_height;
                    auto rgb = ocr_band_capture->capture(*images.out_image, band_width, band_height);
                    ocr_worker->submitBand(std::move(rgb), band_width, band_height,
                                           state.stream_seconds);
                }
                for (const auto &update : ocr_worker->drainUpdates()) {
                    // Log with the subtitle's absolute position in the capture
                    // (independent of --seek) so a specific cue is easy to
                    // refer to
                    if (!update.lines.empty()) {
                        std::string text;
                        for (const auto &line : update.lines) {
                            if (!text.empty()) text += " | ";
                            text += line;
                        }
                        const double file_seconds = state.stream_start_seconds + update.seconds;
                        const long msec = std::lround(std::abs(file_seconds) * 1000);
                        log.info(eApplication,
                                 std::format("OCR [{}{:02}:{:02}.{}] {}",
                                             file_seconds < 0 ? "-" : "", msec / 60000,
                                             msec / 1000 % 60, msec % 1000 / 100, text));
                    }
                    ocr_feed.apply(update, state.stream_seconds,
                                   (*subtitle_tracks)[ocr_track_index], *subtitle_font);
                    if (ocr_translator) {
                        const long id = ocr_translated_feed.onOcrUpdate(
                                update, (*subtitle_tracks)[ocr_en_track_index]);
                        if (id) ocr_translator->submit(id, update.lines);
                    }
                }
                if (ocr_translator) {
                    for (const auto &result : ocr_translator->drainResults()) {
                        auto applied = ocr_translated_feed.onTranslation(
                                result, state.stream_seconds,
                                (*subtitle_tracks)[ocr_en_track_index], *subtitle_font);
                        if (applied && !result.lines.empty()) {
                            std::string text;
                            for (const auto &line : result.lines) {
                                if (!text.empty()) text += " | ";
                                text += line;
                            }
                            const double file_seconds = state.stream_start_seconds + applied->start;
                            const long msec = std::lround(std::abs(file_seconds) * 1000);
                            const std::string late = applied->late_seconds > 0
                                ? std::format(" (cue ended {:.1f} s before the translation "
                                              "arrived; showing it briefly)", applied->late_seconds)
                                : "";
                            log.info(eApplication,
                                     std::format("{} [{}{:02}:{:02}.{}] {}{}",
                                                 (*subtitle_tracks)[ocr_en_track_index].label,
                                                 file_seconds < 0 ? "-" : "", msec / 60000,
                                                 msec / 1000 % 60, msec % 1000 / 100, text, late));
                        }
                    }
                }
                subtitle_font->refreshAtlasIfDirty(command_pool);
            }
#endif

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

#ifdef HAVE_OCR
        // Save the collected cues with their original imprint timing, as
        // absolute positions in the input so the files line up when played
        // from the start (--subtitles auto-discovers them by these names)
        if (ocr_worker && !subtitle_setup.ocr_write_stem.empty() && subtitle_tracks) {
            auto absoluteEntries = [&](std::vector<SubtitleEntry> entries) {
                for (auto &e : entries) {
                    e.start_seconds += initial_seek_seconds;
                    e.end_seconds += initial_seek_seconds;
                }
                std::sort(entries.begin(), entries.end(),
                          [](const auto &a, const auto &b) {
                              return a.start_seconds < b.start_seconds;
                          });
                return entries;
            };
            try {
                ocr_feed.finish(state.stream_seconds, (*subtitle_tracks)[ocr_track_index]);
                const std::string ja_path = subtitle_setup.ocr_write_stem + ".OCR.srt";
                writeSrt(ja_path, absoluteEntries(ocr_feed.file_entries));
                log.info(eApplication, std::format("Wrote {} OCR cues to {}",
                                                   ocr_feed.file_entries.size(), ja_path));
                if (ocr_translator) {
                    ocr_translated_feed.finish(state.stream_seconds,
                                               (*subtitle_tracks)[ocr_en_track_index]);
                    const std::string en_path = subtitle_setup.ocr_write_stem + ".OCR-"
                            + languageCode(subtitle_setup.ocr_target_language) + ".srt";
                    writeSrt(en_path, absoluteEntries(ocr_translated_feed.file_entries));
                    log.info(eApplication, std::format("Wrote {} translated cues to {}",
                                                       ocr_translated_feed.file_entries.size(),
                                                       en_path));
                }
            } catch (const std::exception &x) {
                log.error(eApplication, std::format("Saving OCR subtitles failed: {}", x.what()));
            }
        }
#endif

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
                  double write_duration_seconds,
                  double initial_seek_seconds) {
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

    // Teardown must run in this order on every exit path, including stack
    // unwinding: first stop the reader, which joins the reader/demodulator/EFM
    // threads while the Vulkan device they use is still alive; then tear down
    // Vulkan, which destroys the surface; then the window the surface was
    // created from.  Both cleanup()s are idempotent, and their destructors
    // would otherwise run too late -- reader and manager belong to main().
    // Declared before initVulkan so a failure anywhere below is covered.
    struct Teardown {
        Logger &log;
        FrameReader<InputBlock> &reader;
        musevk::VulkanManager &manager;
        GLFWwindow *window;
        ~Teardown() {
            try {
                reader.cleanup();
                manager.cleanup();
            } catch (const std::exception &x) {
                log.error(eApplication, x.what());
            }
            glfwDestroyWindow(window);
            glfwTerminate();
        }
    } teardown{log, reader, manager, window};

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
                  export_frame_filename, export_frame_after_seconds, write_duration_seconds, seconds_per_iteration,
                  initial_seek_seconds);
    }

#ifdef HAVE_LIBAV
    if (vfw) {
        vfw->cleanup();
        vfw = nullptr;
    }
#endif
    // audio_playback, the decoder scope above and the Teardown guard handle the
    // rest, in reverse declaration order.
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
    double subtitle_offset_seconds = 0.0;
    optional<string> ocr_models_dir;
    optional<string> ocr_translate_url;
    string ocr_translate_model;
    string ocr_script = "cjk";
    string ocr_source_language = "Japanese";
    string ocr_target_language = "English";
    bool ocr_write = false;

    const vector<string> args(argv + 1, argv + argc);
    auto it = args.cbegin();

    CliOptions options;

    auto usage = [&options] (ostream &out, int status) -> void {
        options.printHelp(out, "museld [options] <input_file> ...");
        out << "\nSeveral input files can be given, with options in between; each one is played with\n"
               "the options in effect where it appears.  Options therefore apply to the files that\n"
               "follow them: one placed after a filename does not affect that file, and options\n"
               "after the last filename do nothing at all.  An argument starting with ! is ignored,\n"
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
    options.flag("--subtitles", "Display SRT subtitles synced to the disc's own time code, or to "
                                "the playback position when the capture carries no disc code "
                                "(baseband captures usually do not).  The "
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
    options.option("--subtitle-offset", "SECONDS", "Delay the subtitles by this much; negative "
                                                   "shows them earlier.  Useful for tracks timed "
                                                   "against a --write render rather than the "
                                                   "disc's own time code", [&] () -> void {
        subtitle_offset_seconds = stod(*(it++));
    });
    options.option("--ocr", "DIR", "OCR burned-in Japanese subtitles during playback into a "
                                   "live \"OCR\" subtitle track, selectable like any other with "
                                   "the [ and ] keys.  DIR holds the PP-OCR text detection and "
                                   "recognition models (the .onnx files with \"det\" and \"rec\" "
                                   "in their names)", [&] () -> void {
        ocr_models_dir = *(it++);
#ifndef HAVE_OCR
        cerr << "--ocr requires a build with -DUSE_OCR=ON" << endl;
        exit(EXIT_FAILURE);
#endif
        if (!filesystem::is_directory(*ocr_models_dir)) {
            cerr << "OCR model directory not found: " << *ocr_models_dir << endl;
            exit(EXIT_FAILURE);
        }
    });
    options.option("--ocr-translate", "URL", "Translate the OCR track into a live \"OCR-EN\" "
                                             "track via the OpenAI-compatible server at URL "
                                             "(llama.cpp, Ollama, vLLM, ...; e.g. "
                                             "http://localhost:11434).  $OPENAI_API_KEY is sent "
                                             "as a bearer token if set.  Requires --ocr",
                   [&] () -> void {
        ocr_translate_url = *(it++);
    });
    options.option("--ocr-translate-model", "NAME", "Model for --ocr-translate (default: the "
                                                    "first one the server reports)", [&] () -> void {
        ocr_translate_model = *(it++);
    });
    options.option("--ocr-script", "SCRIPT", "Script a text row must contain to count as a "
                                             "subtitle: cjk (default; Japanese and Chinese), "
                                             "latin, or any (keep everything, e.g. credits)",
                   [&] () -> void {
        ocr_script = *(it++);
        if (ocr_script != "cjk" && ocr_script != "latin" && ocr_script != "any") {
            cerr << "--ocr-script must be cjk, latin or any" << endl;
            exit(EXIT_FAILURE);
        }
    });
    options.option("--ocr-language", "NAME", "Language of the burned-in subtitles, for the "
                                             "translation prompt (default Japanese).  Pick a "
                                             "matching recognition model for --ocr and "
                                             "--ocr-script accordingly", [&] () -> void {
        ocr_source_language = *(it++);
    });
    options.option("--ocr-translate-to", "NAME", "Language to translate the subtitles into "
                                                 "(default English).  The live track and saved "
                                                 "file are named after its first two letters",
                   [&] () -> void {
        ocr_target_language = *(it++);
    });
    options.flag("--ocr-write", "Save the cues collected by --ocr at exit, with their original "
                                "imprint timing, as <input>.OCR.srt (and <input>.OCR-EN.srt with "
                                "--ocr-translate) next to the input file, where --subtitles "
                                "finds them on the next playback", [&] () -> void {
        ocr_write = true;
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
        // Options apply to the input files that follow them, so any left over when
        // the arguments run out did nothing.  Remember them to say so afterwards.
        vector<string> trailing_options;
        while (it != args.cend()) {
            if (const CliOptions::Option *option = options.find(*it)) {
                it++;
                if (option->takesArgument() && it == args.cend())
                    throw runtime_error(std::format("{} needs an argument ({})", option->name, option->argument));
                option->action();
                trailing_options.push_back(option->name);
            } else if (it->find("!", 0) == 0) {
                it++; // used to ignore options (to easily enable/disable options in CLion debug settings)
            } else if (it->find("-", 0) == 0) {
                cerr << "Unknown option: " << *it << endl;
                usage(cerr, EXIT_FAILURE);
            } else {
                input_file_given = true;
                trailing_options.clear(); // these applied to this file
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
                subtitle_setup.offset_seconds = subtitle_offset_seconds;
                subtitle_setup.ocr_models_dir = ocr_models_dir;
                if (ocr_translate_url && !ocr_models_dir) {
                    cerr << "--ocr-translate requires --ocr" << endl;
                    exit(EXIT_FAILURE);
                }
                subtitle_setup.ocr_translate_url = ocr_translate_url;
                subtitle_setup.ocr_translate_model = ocr_translate_model;
                if (const char *key = getenv("OPENAI_API_KEY"))
                    subtitle_setup.ocr_translate_key = key;
                subtitle_setup.ocr_script = ocr_script;
                subtitle_setup.ocr_source_language = ocr_source_language;
                subtitle_setup.ocr_target_language = ocr_target_language;
                if (ocr_write) {
                    if (!ocr_models_dir) {
                        cerr << "--ocr-write requires --ocr" << endl;
                        exit(EXIT_FAILURE);
                    }
                    const filesystem::path input_path(*it);
                    subtitle_setup.ocr_write_stem =
                            (input_path.parent_path() / input_path.stem()).string();
                }
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
                        auto reader = make_unique<NtscFrameReader>(
                                        log, executable_dir, manager, *it, input_format,
                                        input_sample_frequency, initial_seek_seconds, benchmark_shaders, efm_audio,
                                        muse_output_filename);
                        process_file<NtscInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                                     efm_audio,
                                                     benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds,
                                     initial_seek_seconds);
                        break;
                    }
                    case eMuse16MHz: {
                        auto reader = make_unique<PhaseCorrect16MHzFrameReader>(
                                log, *it, input_format, initial_seek_seconds, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds,
                                     initial_seek_seconds);
                        break;
                    }
                    case eMuseOversampled:
                    case eMuseRf: {
                        auto reader = make_unique<ResamplingFrameReader>(
                                log, executable_dir, manager, *it, input_format,
                                input_sample_frequency, initial_seek_seconds, input_type == eMuseRf, benchmark_shaders,
                                efm_audio, muse_output_filename);
                        process_file<MuseInputBlock>(log, executable_dir, manager, *reader, decode_all_fields,
                                     full_screen, no_sync, start_paused, field_interpolation_mode, use_3d_comb, film_mode, decode_video, dropout_mode, decode_audio,
                                     efm_audio, benchmark_shaders, eq_mode, eq_alpha, tint_degrees, saturation, output_filename, write_preset,
                                     subtitle_setup,
                                     export_frame_filename, export_frame_after_seconds, write_duration_seconds,
                                     initial_seek_seconds);
                        break;
                    }
                    default:
                        throw std::runtime_error("Unknown input type: {}");
                }
                it++;
            }
        }
        // Options are applied to the files that follow them, so trailing ones were
        // parsed and then never used.  That is legal syntax -- it is how a second
        // file is given different settings -- so it cannot be an error, but it is
        // almost always a mistake worth pointing out.
        if (input_file_given && !trailing_options.empty()) {
            string names;
            for (const auto &name : trailing_options)
                names += (names.empty() ? "" : " ") + name;
            cerr << std::format("Warning: {} came after the last input file and had no effect: {}\n"
                                "Options apply to the input files that follow them, so they belong "
                                "before the file they are meant for.",
                                trailing_options.size() == 1 ? "this option" : "these options", names)
                 << endl;
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
