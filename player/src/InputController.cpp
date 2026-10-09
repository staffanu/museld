// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "InputController.h"

#include <algorithm>
#include <format>

#include <GLFW/glfw3.h>

#include "PlayerState.h"
#include "logging/Logger.h"

bool startChapterSearch(PlayerState &state, const ReaderControls &reader, Logger &log,
                        ChapterSearch::Direction direction, std::optional<int> target_chapter) {
    if (state.chapter_search)
        return false; // one at a time
    const auto &info = state.last_decoded.disc_info;
    if (!info || !info->chapter()) {
        state.osd_text = "NO CHAPTER CODE";
        return false;
    }
    if (!reader.seekToInputSample || reader.samples_per_second <= 0) {
        state.osd_text = "SEARCH NOT AVAIL";
        return false;
    }
    const ChapterSearch::Reading here{state.last_decoded.disc_info_input_offset, info->chapter(),
                                      info->isLeadIn(), info->isLeadOut()};
    const ChapterSearch::Params params{reader.samples_per_second,
                                       reader.input_sample_count ? reader.input_sample_count() : -1};
    state.chapter_search = target_chapter
            ? std::make_unique<ChapterSearch>(params, here, *target_chapter)
            : std::make_unique<ChapterSearch>(params, here, direction);
    state.search_action_pending = true;
    // The search needs the decoder running; back to the pause when done.
    // The OSD text is drawn into the displayed image, which the search
    // leaves as it is, so re-decode it once first: that clears whatever
    // text is on it before "SEARCH" goes on.
    state.search_resume_paused = state.paused;
    state.paused = false;
    state.redo_last_field = true;
    state.paused_countdown = 0;
    log.info(eApplication, std::format("Chapter search: {} from chapter {} at input sample {}",
                                       target_chapter ? std::format("chapter {}", *target_chapter)
                                       : direction == ChapterSearch::Direction::eNext ? "next" : "previous",
                                       *info->chapter(), here.offset));
    return true;
}

bool InputController::checkKey(GLFWwindow *window, int key) {
    if (glfwGetKey(window, key) == GLFW_PRESS) {
        if (m_keys_down.find(key) == m_keys_down.end()) {
            m_keys_down.insert(key);
            return true;
        }
        return false;
    }
    m_keys_down.erase(key);
    return false;
}

bool InputController::poll(GLFWwindow *window,
                           PlayerState &state,
                           ReaderControls &reader,
                           DropoutMode &dropout_mode,
                           AudioTrack &audio_track,
                           bool &full_screen,
                           int window_width,
                           int window_height) {
    glfwPollEvents();

    if (checkKey(window, GLFW_KEY_ESCAPE) || checkKey(window, GLFW_KEY_Q))
        return false;
    if (checkKey(window, GLFW_KEY_TAB)) {
        if (full_screen) {
            // Back to the size the window had, or the caller's default when
            // playback started full screen
            const int w = m_windowed_width > 0 ? m_windowed_width : window_width;
            const int h = m_windowed_height > 0 ? m_windowed_height : window_height;
            glfwSetWindowMonitor(window, nullptr, 0, 0, w, h, GLFW_DONT_CARE);
            full_screen = false;
        } else {
            // The monitor's current mode: no mode switch, the picture is
            // scaled to the screen by the blit
            glfwGetWindowSize(window, &m_windowed_width, &m_windowed_height);
            GLFWmonitor *monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
            full_screen = true;
        }
    }
    if (checkKey(window, GLFW_KEY_SPACE)) {
        state.paused = !state.paused;
        state.osd_text = state.paused ? "PAUSE" : "PLAY";
    }
    if (checkKey(window, GLFW_KEY_N)) {
        state.paused = false;
        state.redo_last_field = false;
        state.paused_countdown = 1;
    }

    const double zoom_step = 0.2;
    if (checkKey(window, GLFW_KEY_LEFT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                               state.zoom_center.first - zoom_step / state.zoom_factor);
        } else if (!reader.seek(-10)) {
            state.osd_text = "SEEK FAILED";
        } else {
            // Keep the stream clock (subtitle fallback time base) in step.  The
            // input readers clamp backward seeks at the start of the input, so
            // clamp the clock the same way; stream_seconds goes negative when
            // seeking back across the initial --seek position, matching the file.
            state.stream_seek_offset_seconds +=
                    std::max(-10.0, -(state.stream_start_seconds + state.stream_seconds));
            if (state.paused) {
                state.paused = false;
                state.paused_countdown = 5;
            }
        }
    }
    if (checkKey(window, GLFW_KEY_RIGHT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::min(1.0 - 0.5 / state.zoom_factor,
                                               state.zoom_center.first + zoom_step / state.zoom_factor);
        } else if (!reader.seek(10)) {
            state.osd_text = "SEEK FAILED";
        } else {
            state.stream_seek_offset_seconds += 10.0;
            if (state.paused) {
                state.paused = false;
                state.paused_countdown = 5;
            }
        }
    }
    // Up/Down: the next/previous chapter, found by probing the input (see
    // ChapterSearch; the loop in runPlayer drives it).  Like Left/Right
    // they pan instead while zoomed.
    if (checkKey(window, GLFW_KEY_UP)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                                state.zoom_center.second - zoom_step / state.zoom_factor);
        } else {
            startChapterSearch(state, reader, m_log, ChapterSearch::Direction::eNext);
        }
    }
    if (checkKey(window, GLFW_KEY_DOWN)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.second = std::min(1.0 - 0.5 / state.zoom_factor,
                                                state.zoom_center.second + zoom_step / state.zoom_factor);
        } else {
            startChapterSearch(state, reader, m_log, ChapterSearch::Direction::ePrevious);
        }
    }
    if (checkKey(window, GLFW_KEY_P)) {
        state.honor_picture_stops = !state.honor_picture_stops;
        state.osd_text = state.honor_picture_stops ? "PICTURE STOPS ON" : "PICTURE STOPS OFF";
    }

    if (checkKey(window, GLFW_KEY_1)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation determined by motion detection");
        state.osd_text = "MOTION NORMAL";
    }
    if (checkKey(window, GLFW_KEY_2)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation forced to intra field only");
        state.osd_text = "MOTION ALL";
    }
    if (checkKey(window, GLFW_KEY_3)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Inter-frame interpolation forced");
        state.osd_text = "MOTION NONE";
    }
    if (checkKey(window, GLFW_KEY_4)) {
        state.use_3d_comb = !state.use_3d_comb;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.use_3d_comb ? "3D comb enabled" : "3D comb disabled");
        state.osd_text = state.use_3d_comb ? "3D COMB ON" : "3D COMB OFF";
    }
    if (checkKey(window, GLFW_KEY_5)) {
        state.film_mode = !state.film_mode;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.film_mode ? "Film mode auto" : "Film mode off");
        state.osd_text = state.film_mode ? "FILM MODE AUTO" : "FILM MODE OFF";
    }
    if (checkKey(window, GLFW_KEY_A)) {
        // Cycle through the disc's audio tracks: default -> EFM -> (AC3 on
        // NTSC) -> default
        audio_track = audio_track == AudioTrack::eDefault ? AudioTrack::eEfm
                    : audio_track == AudioTrack::eEfm && reader.has_ac3_audio ? AudioTrack::eAc3
                    : AudioTrack::eDefault;
        reader.setAudioTrack(audio_track);
        state.osd_text = audio_track == AudioTrack::eEfm ? "EFM AUDIO"
                       : audio_track == AudioTrack::eAc3 ? reader.ac3_audio_label
                       : reader.default_audio_label;
    }
    if (checkKey(window, GLFW_KEY_B)) {
        switch (state.audio_channel_mode) {
            case AudioChannelMode::eStereo:
                state.audio_channel_mode = AudioChannelMode::eLeft;
                state.osd_text = "LEFT CHANNEL";
                break;
            case AudioChannelMode::eLeft:
                state.audio_channel_mode = AudioChannelMode::eRight;
                state.osd_text = "RIGHT CHANNEL";
                break;
            case AudioChannelMode::eRight:
                state.audio_channel_mode = AudioChannelMode::eStereo;
                state.osd_text = "STEREO";
                break;
        }
    }
    if (checkKey(window, GLFW_KEY_X)) {
        switch (state.analog_cx_mode) {
            case Decoder::CxMode::eAuto:
                state.analog_cx_mode = Decoder::CxMode::eOff;
                state.osd_text = "CX OFF";
                break;
            case Decoder::CxMode::eOff:
                state.analog_cx_mode = Decoder::CxMode::eOn;
                state.osd_text = "CX ON";
                break;
            case Decoder::CxMode::eOn:
                state.analog_cx_mode = Decoder::CxMode::eAuto;
                state.osd_text = "CX AUTO";
                break;
        }
        // Parenthesized when the audio playing is not the NTSC analog track,
        // where the setting is remembered but has no audible effect
        if (!reader.has_analog_audio || audio_track != AudioTrack::eDefault)
            state.osd_text = "(" + state.osd_text + ")";
    }
    if (checkKey(window, GLFW_KEY_J)) {
        // NTSC-M (US) and NTSC-J discs put black 7.5 and 0 IRE above blanking
        switch (state.black_level_mode) {
            case Decoder::BlackLevelMode::eAuto:
                state.black_level_mode = Decoder::BlackLevelMode::eM;
                state.osd_text = "BLACK 7.5 IRE (NTSC-M)";
                break;
            case Decoder::BlackLevelMode::eM:
                state.black_level_mode = Decoder::BlackLevelMode::eJ;
                state.osd_text = "BLACK 0 IRE (NTSC-J)";
                break;
            case Decoder::BlackLevelMode::eJ:
                state.black_level_mode = Decoder::BlackLevelMode::eAuto;
                state.osd_text = "BLACK AUTO";
                break;
        }
        if (state.paused) state.redo_last_field = true;
    }
    if (checkKey(window, GLFW_KEY_D)) {
        switch (dropout_mode) {
            case DropoutMode::eNormal:
                dropout_mode = DropoutMode::eDisabled;
                state.osd_text = "DROPOUT DISABLED";
                break;
            case DropoutMode::eDisabled:
                dropout_mode = DropoutMode::eHighlight;
                state.osd_text = "DROPOUT HIGHLIGHT";
                break;
            case DropoutMode::eHighlight:
                dropout_mode = DropoutMode::eNormal;
                state.osd_text = "DROPOUT ENABLED";
                break;
        }
    }
    if (checkKey(window, GLFW_KEY_L)) {
        state.enable_non_linear = !state.enable_non_linear;
        state.osd_text = state.enable_non_linear ? "NON-LINEAR DE-EMPH ON" : "NON-LINEAR DE-EMPH OFF";
    }
    if (checkKey(window, GLFW_KEY_C)) {
        state.enable_cursor = !state.enable_cursor;
        glfwSetInputMode(window, GLFW_CURSOR, state.enable_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }
    if (checkKey(window, GLFW_KEY_V)) {
        state.show_disc_code = !state.show_disc_code;
    }
    if (checkKey(window, GLFW_KEY_PRINT_SCREEN)) {
        glfwSetClipboardString(window, state.last_cursor_string.c_str());
    }
    if (checkKey(window, GLFW_KEY_S)) {
        state.export_frame = true;
        if (state.paused)
            state.redo_last_field = true; // re-decode so baked-in OSD text is not exported
    }
    if (checkKey(window, GLFW_KEY_7) && reader.cycleEqMode) {
        std::string mode = reader.cycleEqMode();
        state.osd_text = std::format("EQ {}", mode);
        m_log.info(eApplication | eVideo, std::format("Adaptive equaliser mode: {}", mode));
    }
    if (checkKey(window, GLFW_KEY_8) && reader.resetEqTaps) {
        reader.resetEqTaps();
        state.osd_text = "EQ RESET";
        m_log.info(eApplication | eVideo, "Adaptive equaliser taps reset to identity");
    }
    if (checkKey(window, GLFW_KEY_E) && reader.efmFilterSize && reader.setEfmFilterSize) {
        // Cycle the EFM adaptive filter through roughly doubling sizes -- some
        // discs need a very long filter -- and then off.  A size set on the
        // command line that is not in the list steps to the next larger one;
        // from off (or above the largest) the cycle restarts at 3.
        static constexpr int sizes[] = { 3, 5, 9, 17, 35, 71 };
        const int current = reader.efmFilterSize();
        int next = 0;
        if (current == 0) {
            next = sizes[0];
        } else {
            for (int s : sizes)
                if (s > current) { next = s; break; }
        }
        reader.setEfmFilterSize(next);
        state.osd_text = next == 0 ? "EFM FILTER OFF" : std::format("EFM FILTER {} TAPS", next);
        m_log.info(eApplication | eAudio, std::format("EFM adaptive filter size: {}", next));
    }
    auto cycleSubtitleSlot = [&](int &slot, const char *osd_prefix) {
        const int n = static_cast<int>(state.subtitle_track_names.size());
        if (n == 0) return;
        slot = slot + 1 >= n ? -1 : slot + 1;
        state.osd_text = slot < 0 ? std::format("{} OFF", osd_prefix)
                                  : std::format("{} {}", osd_prefix, state.subtitle_track_names[slot]);
    };
    if (checkKey(window, GLFW_KEY_LEFT_BRACKET))
        cycleSubtitleSlot(state.subtitle_primary, "SUBTITLES");
    if (checkKey(window, GLFW_KEY_RIGHT_BRACKET))
        cycleSubtitleSlot(state.subtitle_secondary, "SUBTITLES 2");
    if (checkKey(window, GLFW_KEY_F)) {
        // Cycle the picture format; SQUEEZE only applies to a 4:3 frame
        switch (state.aspect_mode) {
            case AspectMode::eNormal:
                state.aspect_mode = AspectMode::eZoom;
                break;
            case AspectMode::eZoom:
                state.aspect_mode = state.picture_format.can_squeeze ? AspectMode::eSqueeze : AspectMode::eStretch;
                break;
            case AspectMode::eSqueeze:
                state.aspect_mode = AspectMode::eStretch;
                break;
            case AspectMode::eStretch:
                state.aspect_mode = AspectMode::eNormal;
                break;
        }
        state.osd_text = std::format("ASPECT {}", aspectModeName(state.aspect_mode));
    }
    if (checkKey(window, GLFW_KEY_O)) {
        // How much of the edges to hide: the standard picture, a TV's
        // overscan on top of it, or nothing at all (the whole decoded image,
        // which for NTSC includes the blanking margins at the sides)
        const bool has_margins = state.picture_format.picture_x0 > 0
                                 || state.picture_format.picture_x1 < state.picture_format.width;
        if (state.full_image) {
            state.full_image = false;
            state.overscan = 0.0;
        } else if (state.overscan < 0.0249) {
            state.overscan = 0.025;
        } else if (state.overscan < 0.0499) {
            state.overscan = 0.05;
        } else if (has_margins) {
            state.full_image = true;
        } else {
            state.overscan = 0.0;
        }
        state.osd_text = state.full_image ? "OVERSCAN OFF: FULL IMAGE"
                                          : std::format("OVERSCAN {:g}%", state.overscan * 100);
    }
    if (checkKey(window, GLFW_KEY_Z)) {
        state.zoom_factor = (state.zoom_factor * 2) % 7;
        state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                           std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.first));
        state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                            std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.second));
        state.osd_text = std::format("ZOOM {}", state.zoom_factor);
    }
    return true;
}
