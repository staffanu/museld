// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "InputController.h"

#include <algorithm>
#include <format>

#include <SDL3/SDL.h>

#include "PlayerState.h"
#include "logging/Logger.h"

bool InputController::checkKey(int scancode) const {
    return m_pressed.contains(scancode);
}

bool InputController::poll(SDL_Window *window,
                           PlayerState &state,
                           ReaderControls &reader,
                           DropoutMode &dropout_mode,
                           AudioTrack &audio_track,
                           bool &full_screen) {
    // Drain the event queue: the keys pressed since the last poll, and whether
    // the window was closed.  Scancodes name the physical key, so the
    // bindings sit where they do on a US keyboard whatever the layout.
    m_pressed.clear();
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat)
                    m_pressed.insert(event.key.scancode);
                break;
            default:
                break;
        }
    }
    if (quit || checkKey(SDL_SCANCODE_ESCAPE) || checkKey(SDL_SCANCODE_Q))
        return false;
    if (checkKey(SDL_SCANCODE_TAB)) {
        // Full screen at the desktop's mode -- no mode switch, the picture is
        // scaled to the screen by the blit; SDL restores the windowed size
        full_screen = !full_screen;
        if (!SDL_SetWindowFullscreen(window, full_screen))
            m_log.warn(eApplication, std::format("SDL_SetWindowFullscreen: {}", SDL_GetError()));
    }
    if (checkKey(SDL_SCANCODE_SPACE)) {
        state.paused = !state.paused;
        state.osd_text = state.paused ? "PAUSE" : "PLAY";
    }
    if (checkKey(SDL_SCANCODE_N)) {
        state.paused = false;
        state.redo_last_field = false;
        state.paused_countdown = 1;
    }

    const double zoom_step = 0.2;
    if (checkKey(SDL_SCANCODE_LEFT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                               state.zoom_center.first - zoom_step / state.zoom_factor);
        } else {
            reader.seek(-10);
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
    if (checkKey(SDL_SCANCODE_RIGHT)) {
        if (state.zoom_factor != 1) {
            state.zoom_center.first = std::min(1.0 - 0.5 / state.zoom_factor,
                                               state.zoom_center.first + zoom_step / state.zoom_factor);
        } else {
            reader.seek(10);
            state.stream_seek_offset_seconds += 10.0;
            if (state.paused) {
                state.paused = false;
                state.paused_countdown = 5;
            }
        }
    }
    if (checkKey(SDL_SCANCODE_UP)) {
        state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                            state.zoom_center.second - zoom_step / state.zoom_factor);
    }
    if (checkKey(SDL_SCANCODE_DOWN)) {
        state.zoom_center.second = std::min(1.0 - 0.5 / state.zoom_factor,
                                            state.zoom_center.second + zoom_step / state.zoom_factor);
    }

    if (checkKey(SDL_SCANCODE_1)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation determined by motion detection");
        state.osd_text = "MOTION NORMAL";
    }
    if (checkKey(SDL_SCANCODE_2)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceIntraField;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Field interpolation forced to intra field only");
        state.osd_text = "MOTION ALL";
    }
    if (checkKey(SDL_SCANCODE_3)) {
        state.field_interpolation_mode = Decoder::FieldInterpolationMode::eForceInterFrame;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, "Inter-frame interpolation forced");
        state.osd_text = "MOTION NONE";
    }
    if (checkKey(SDL_SCANCODE_4)) {
        state.use_3d_comb = !state.use_3d_comb;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.use_3d_comb ? "3D comb enabled" : "3D comb disabled");
        state.osd_text = state.use_3d_comb ? "3D COMB ON" : "3D COMB OFF";
    }
    if (checkKey(SDL_SCANCODE_5)) {
        state.film_mode = !state.film_mode;
        if (state.paused) state.redo_last_field = true;
        m_log.info(eApplication | eVideo, state.film_mode ? "Film mode auto" : "Film mode off");
        state.osd_text = state.film_mode ? "FILM MODE AUTO" : "FILM MODE OFF";
    }
    if (checkKey(SDL_SCANCODE_A)) {
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
    if (checkKey(SDL_SCANCODE_B)) {
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
    if (checkKey(SDL_SCANCODE_X)) {
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
    if (checkKey(SDL_SCANCODE_J)) {
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
    if (checkKey(SDL_SCANCODE_D)) {
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
    if (checkKey(SDL_SCANCODE_L)) {
        state.enable_non_linear = !state.enable_non_linear;
        state.osd_text = state.enable_non_linear ? "NON-LINEAR DE-EMPH ON" : "NON-LINEAR DE-EMPH OFF";
    }
    if (checkKey(SDL_SCANCODE_C)) {
        state.enable_cursor = !state.enable_cursor;
        if (state.enable_cursor)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
    }
    if (checkKey(SDL_SCANCODE_V)) {
        state.show_disc_code = !state.show_disc_code;
    }
    if (checkKey(SDL_SCANCODE_PRINTSCREEN)) {
        SDL_SetClipboardText(state.last_cursor_string.c_str());
    }
    if (checkKey(SDL_SCANCODE_S)) {
        state.export_frame = true;
        if (state.paused)
            state.redo_last_field = true; // re-decode so baked-in OSD text is not exported
    }
    if (checkKey(SDL_SCANCODE_7) && reader.cycleEqMode) {
        std::string mode = reader.cycleEqMode();
        state.osd_text = std::format("EQ {}", mode);
        m_log.info(eApplication | eVideo, std::format("Adaptive equaliser mode: {}", mode));
    }
    if (checkKey(SDL_SCANCODE_8) && reader.resetEqTaps) {
        reader.resetEqTaps();
        state.osd_text = "EQ RESET";
        m_log.info(eApplication | eVideo, "Adaptive equaliser taps reset to identity");
    }
    if (checkKey(SDL_SCANCODE_E) && reader.efmFilterSize && reader.setEfmFilterSize) {
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
    if (checkKey(SDL_SCANCODE_LEFTBRACKET))
        cycleSubtitleSlot(state.subtitle_primary, "SUBTITLES");
    if (checkKey(SDL_SCANCODE_RIGHTBRACKET))
        cycleSubtitleSlot(state.subtitle_secondary, "SUBTITLES 2");
    if (checkKey(SDL_SCANCODE_F)) {
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
    if (checkKey(SDL_SCANCODE_O)) {
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
    if (checkKey(SDL_SCANCODE_Z)) {
        state.zoom_factor = (state.zoom_factor * 2) % 7;
        state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                           std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.first));
        state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                            std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.second));
        state.osd_text = std::format("ZOOM {}", state.zoom_factor);
    }
    return true;
}
