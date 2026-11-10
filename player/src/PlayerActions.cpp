// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <format>
#include <SDL3/SDL.h>
#include "PlayerActions.h"
#include "logging/Logger.h"

namespace {

// Steps an index through n states in either direction
int stepped(int index, int n, int direction) {
    return ((index + (direction >= 0 ? 1 : -1)) % n + n) % n;
}

void redoIfPaused(PlayerState &state) {
    if (state.paused)
        state.redo_last_field = true;
}

} // namespace

namespace PlayerActions {

void togglePause(PlayerState &state) {
    state.paused = !state.paused;
    state.osd_text = state.paused ? "PAUSE" : "PLAY";
}

void stepOneField(PlayerState &state) {
    state.paused = false;
    state.redo_last_field = false;
    state.paused_countdown = 1;
}

void seekOrPan(PlayerState &state, ReaderControls &reader, double seconds) {
    const double zoom_step = 0.2;
    if (state.zoom_factor != 1) {
        const double step = (seconds < 0 ? -zoom_step : zoom_step) / state.zoom_factor;
        state.zoom_center.first = std::clamp(state.zoom_center.first + step,
                                             0.5 / state.zoom_factor, 1.0 - 0.5 / state.zoom_factor);
        return;
    }
    reader.seek(seconds);
    // Keep the stream clock (subtitle fallback time base) in step.  The
    // input readers clamp backward seeks at the start of the input, so
    // clamp the clock the same way; stream_seconds goes negative when
    // seeking back across the initial --seek position, matching the file.
    if (seconds < 0)
        state.stream_seek_offset_seconds +=
                std::max(seconds, -(state.stream_start_seconds + state.stream_seconds));
    else
        state.stream_seek_offset_seconds += seconds;
    if (state.paused) {
        state.paused = false;
        state.paused_countdown = 5;
    }
}

void chapterOrPan(PlayerState &state, ReaderControls &reader, Logger &log, bool next) {
    const double zoom_step = 0.2;
    if (state.zoom_factor != 1) {
        const double step = (next ? -zoom_step : zoom_step) / state.zoom_factor;
        state.zoom_center.second = std::clamp(state.zoom_center.second + step,
                                              0.5 / state.zoom_factor, 1.0 - 0.5 / state.zoom_factor);
        return;
    }
    startChapterSearch(state, reader, log, next ? ChapterSearch::Direction::eNext : ChapterSearch::Direction::ePrevious);
}

void togglePictureStops(PlayerState &state) {
    state.honor_picture_stops = !state.honor_picture_stops;
    state.osd_text = state.honor_picture_stops ? "PICTURE STOPS ON" : "PICTURE STOPS OFF";
}

void setFieldInterpolation(PlayerState &state, Logger &log, Decoder::FieldInterpolationMode mode) {
    state.field_interpolation_mode = mode;
    redoIfPaused(state);
    switch (mode) {
        case Decoder::FieldInterpolationMode::eNormal:
            log.info(eApplication | eVideo, "Field interpolation determined by motion detection");
            state.osd_text = "MOTION NORMAL";
            break;
        case Decoder::FieldInterpolationMode::eForceIntraField:
            log.info(eApplication | eVideo, "Field interpolation forced to intra field only");
            state.osd_text = "MOTION ALL";
            break;
        case Decoder::FieldInterpolationMode::eForceInterFrame:
            log.info(eApplication | eVideo, "Inter-frame interpolation forced");
            state.osd_text = "MOTION NONE";
            break;
    }
}

void cycleFieldInterpolation(PlayerState &state, Logger &log, int direction) {
    static constexpr Decoder::FieldInterpolationMode modes[] = {
            Decoder::FieldInterpolationMode::eNormal, Decoder::FieldInterpolationMode::eForceIntraField,
            Decoder::FieldInterpolationMode::eForceInterFrame};
    int index = 0;
    for (int i = 0; i < 3; i++)
        if (modes[i] == state.field_interpolation_mode)
            index = i;
    setFieldInterpolation(state, log, modes[stepped(index, 3, direction)]);
}

void toggle3dComb(PlayerState &state, Logger &log) {
    state.use_3d_comb = !state.use_3d_comb;
    redoIfPaused(state);
    log.info(eApplication | eVideo, state.use_3d_comb ? "3D comb enabled" : "3D comb disabled");
    state.osd_text = state.use_3d_comb ? "3D COMB ON" : "3D COMB OFF";
}

void toggleFilmMode(PlayerState &state, Logger &log) {
    state.film_mode = !state.film_mode;
    redoIfPaused(state);
    log.info(eApplication | eVideo, state.film_mode ? "Film mode auto" : "Film mode off");
    state.osd_text = state.film_mode ? "FILM MODE AUTO" : "FILM MODE OFF";
}

void cycleAudioTrack(PlayerState &state, ReaderControls &reader, AudioTrack &audio_track, int direction) {
    // The disc's audio tracks: default -> EFM -> (AC3 on NTSC) -> default
    AudioTrack tracks[3] = {AudioTrack::eDefault, AudioTrack::eEfm, AudioTrack::eAc3};
    const int n = reader.has_ac3_audio ? 3 : 2;
    int index = 0;
    for (int i = 0; i < n; i++)
        if (tracks[i] == audio_track)
            index = i;
    audio_track = tracks[stepped(index, n, direction)];
    reader.setAudioTrack(audio_track);
    state.osd_text = audioTrackName(reader, audio_track);
}

void cycleAudioChannels(PlayerState &state, int direction) {
    static constexpr AudioChannelMode modes[] = {AudioChannelMode::eStereo, AudioChannelMode::eLeft, AudioChannelMode::eRight};
    int index = 0;
    for (int i = 0; i < 3; i++)
        if (modes[i] == state.audio_channel_mode)
            index = i;
    state.audio_channel_mode = modes[stepped(index, 3, direction)];
    state.osd_text = audioChannelsName(state);
}

bool audioChannelChoiceHeard(const PlayerState &state) {
    // The choice applies to two-channel audio; the multichannel modes (4-channel
    // MUSE, 5.1 AC3/DTS) go to the device as they are (see the player loop)
    const AudioMode mode = state.last_decoded.audio_mode;
    return mode != MODE_A && mode != MODE_AC3 && mode != MODE_DTS;
}

void cycleCx(PlayerState &state, const ReaderControls &reader, AudioTrack audio_track, int direction) {
    static constexpr Decoder::CxMode modes[] = {Decoder::CxMode::eAuto, Decoder::CxMode::eOff, Decoder::CxMode::eOn};
    int index = 0;
    for (int i = 0; i < 3; i++)
        if (modes[i] == state.analog_cx_mode)
            index = i;
    state.analog_cx_mode = modes[stepped(index, 3, direction)];
    state.osd_text = cxName(state, reader, audio_track);
}

void cycleBlackLevel(PlayerState &state, int direction) {
    // NTSC-M (US) and NTSC-J discs put black 7.5 and 0 IRE above blanking
    static constexpr Decoder::BlackLevelMode modes[] = {Decoder::BlackLevelMode::eAuto, Decoder::BlackLevelMode::eM, Decoder::BlackLevelMode::eJ};
    int index = 0;
    for (int i = 0; i < 3; i++)
        if (modes[i] == state.black_level_mode)
            index = i;
    state.black_level_mode = modes[stepped(index, 3, direction)];
    state.osd_text = blackLevelName(state.black_level_mode);
    redoIfPaused(state);
}

void cycleDropoutMode(PlayerState &state, DropoutMode &dropout_mode, int direction) {
    static constexpr DropoutMode modes[] = {DropoutMode::eNormal, DropoutMode::eDisabled, DropoutMode::eHighlight};
    int index = 0;
    for (int i = 0; i < 3; i++)
        if (modes[i] == dropout_mode)
            index = i;
    dropout_mode = modes[stepped(index, 3, direction)];
    state.osd_text = dropoutModeName(dropout_mode);
    redoIfPaused(state);
}

void toggleNonLinear(PlayerState &state) {
    state.enable_non_linear = !state.enable_non_linear;
    redoIfPaused(state);
    state.osd_text = state.enable_non_linear ? "NON-LINEAR DE-EMPH ON" : "NON-LINEAR DE-EMPH OFF";
}

void toggleCursor(PlayerState &state) {
    state.enable_cursor = !state.enable_cursor;
    if (state.enable_cursor)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

void toggleDiscCode(PlayerState &state) {
    state.show_disc_code = !state.show_disc_code;
}

void exportFrame(PlayerState &state) {
    state.export_frame = true;
    redoIfPaused(state); // re-decode so baked-in OSD text is not exported
}

void cycleEqMode(PlayerState &state, ReaderControls &reader, Logger &log) {
    if (!reader.cycleEqMode)
        return;
    std::string mode = reader.cycleEqMode();
    state.osd_text = std::format("EQ {}", mode);
    log.info(eApplication | eVideo, std::format("Adaptive equaliser mode: {}", mode));
}

void resetEq(PlayerState &state, ReaderControls &reader, Logger &log) {
    if (!reader.resetEqTaps)
        return;
    reader.resetEqTaps();
    state.osd_text = "EQ RESET";
    log.info(eApplication | eVideo, "Adaptive equaliser taps reset to identity");
}

void cycleEfmFilterSize(PlayerState &state, ReaderControls &reader, Logger &log, int direction) {
    if (!reader.efmFilterSize || !reader.setEfmFilterSize)
        return;
    // Cycle the EFM adaptive filter from the default through the longer sizes,
    // then the short ones, then off (master's order since the 31-tap default).  A
    // size set on the command line that is not in the list steps to the next
    // larger (or, backwards, smaller) one, else off; from off the cycle restarts
    // at the default (backwards: at the last short size).
    static constexpr int sizes[] = { 31, 63, 127, 3, 9, 15 };
    constexpr int n = (int)std::size(sizes);
    const int current = reader.efmFilterSize();
    int at = 0;
    while (at < n && sizes[at] != current) at++;
    int next = 0;
    if (current == 0) {
        next = direction >= 0 ? sizes[0] : sizes[n - 1];
    } else if (at < n) {
        const int to = at + (direction >= 0 ? 1 : -1);
        next = to >= 0 && to < n ? sizes[to] : 0; // off beyond either end
    } else {
        // Not in the list: the nearest size in the direction of travel, else off
        int best = 0;
        for (int s : sizes)
            if (direction >= 0 ? (s > current && (best == 0 || s < best)) : (s < current && s > best))
                best = s;
        next = best;
    }
    reader.setEfmFilterSize(next);
    state.osd_text = next == 0 ? "EFM FILTER OFF" : std::format("EFM FILTER {} TAPS", next);
    log.info(eApplication | eAudio, std::format("EFM adaptive filter size: {}", next));
}

void cycleSubtitleSlot(PlayerState &state, int &slot, const char *osd_prefix, int direction) {
    const int n = static_cast<int>(state.subtitle_track_names.size());
    if (n == 0) return;
    // -1 (off) then the tracks, as a cycle of n + 1 states
    slot = stepped(slot + 1, n + 1, direction) - 1;
    state.osd_text = slot < 0 ? std::format("{} OFF", osd_prefix)
                              : std::format("{} {}", osd_prefix, state.subtitle_track_names[slot]);
}

void cycleAspect(PlayerState &state, int direction) {
    // The picture formats; SQUEEZE only applies to a 4:3 frame
    std::vector<AspectMode> modes = {AspectMode::eNormal, AspectMode::eZoom};
    if (state.picture_format.can_squeeze)
        modes.push_back(AspectMode::eSqueeze);
    modes.push_back(AspectMode::eStretch);
    int index = 0;
    for (size_t i = 0; i < modes.size(); i++)
        if (modes[i] == state.aspect_mode)
            index = (int)i;
    state.aspect_mode = modes[(size_t)stepped(index, (int)modes.size(), direction)];
    state.osd_text = std::format("ASPECT {}", aspectModeName(state.aspect_mode));
}

void cycleOverscan(PlayerState &state, int direction) {
    // How much of the edges to hide: the standard picture, a TV's
    // overscan on top of it, or nothing at all (the whole decoded image,
    // which for NTSC includes the blanking margins at the sides)
    const bool has_margins = state.picture_format.picture_x0 > 0
                             || state.picture_format.picture_x1 < state.picture_format.width;
    // The states in order: 0%, 2.5%, 5%, (full image)
    const int n = has_margins ? 4 : 3;
    int index = state.full_image ? 3 : state.overscan < 0.0249 ? 0 : state.overscan < 0.0499 ? 1 : 2;
    index = stepped(index, n, direction);
    state.full_image = index == 3;
    state.overscan = index == 1 ? 0.025 : index == 2 ? 0.05 : 0.0;
    state.osd_text = overscanName(state);
}

void cycleZoom(PlayerState &state, int direction) {
    // 1, 2, 4
    int index = state.zoom_factor == 1 ? 0 : state.zoom_factor == 2 ? 1 : 2;
    index = stepped(index, 3, direction);
    state.zoom_factor = 1 << index;
    state.zoom_center.first = std::max(0.5 / state.zoom_factor,
                                       std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.first));
    state.zoom_center.second = std::max(0.5 / state.zoom_factor,
                                        std::min(1.0 - 0.5 / state.zoom_factor, state.zoom_center.second));
    state.osd_text = std::format("ZOOM {}", state.zoom_factor);
}

void toggleFullScreen(SDL_Window *window, bool &full_screen, Logger &log) {
    // Full screen at the desktop's mode -- no mode switch, the picture is
    // scaled to the screen by the blit; SDL restores the windowed size
    full_screen = !full_screen;
    if (!SDL_SetWindowFullscreen(window, full_screen))
        log.warn(eApplication, std::format("SDL_SetWindowFullscreen: {}", SDL_GetError()));
}

std::string fieldInterpolationName(Decoder::FieldInterpolationMode mode) {
    switch (mode) {
        case Decoder::FieldInterpolationMode::eNormal: return "MOTION NORMAL";
        case Decoder::FieldInterpolationMode::eForceIntraField: return "MOTION ALL";
        case Decoder::FieldInterpolationMode::eForceInterFrame: return "MOTION NONE";
    }
    return "";
}

std::string audioTrackName(const ReaderControls &reader, AudioTrack track) {
    return track == AudioTrack::eEfm ? "EFM AUDIO"
         : track == AudioTrack::eAc3 ? reader.ac3_audio_label
         : reader.default_audio_label;
}

std::string audioChannelsName(const PlayerState &state) {
    std::string name = state.audio_channel_mode == AudioChannelMode::eStereo ? "STEREO"
                     : state.audio_channel_mode == AudioChannelMode::eLeft ? "LEFT CHANNEL" : "RIGHT CHANNEL";
    // Parenthesized like the CX setting when the audio playing is multichannel
    if (!audioChannelChoiceHeard(state))
        name = "(" + name + ")";
    return name;
}

std::string cxName(const PlayerState &state, const ReaderControls &reader, AudioTrack audio_track) {
    std::string name = state.analog_cx_mode == Decoder::CxMode::eAuto ? "CX AUTO"
                     : state.analog_cx_mode == Decoder::CxMode::eOff ? "CX OFF" : "CX ON";
    // Parenthesized when the audio playing is not the NTSC analog track,
    // where the setting is remembered but has no audible effect
    if (!reader.has_analog_audio || audio_track != AudioTrack::eDefault)
        name = "(" + name + ")";
    return name;
}

std::string blackLevelName(Decoder::BlackLevelMode mode) {
    switch (mode) {
        case Decoder::BlackLevelMode::eAuto: return "BLACK AUTO";
        case Decoder::BlackLevelMode::eM: return "BLACK 7.5 IRE (NTSC-M)";
        case Decoder::BlackLevelMode::eJ: return "BLACK 0 IRE (NTSC-J)";
    }
    return "";
}

std::string dropoutModeName(DropoutMode mode) {
    switch (mode) {
        case DropoutMode::eNormal: return "DROPOUT ENABLED";
        case DropoutMode::eDisabled: return "DROPOUT DISABLED";
        case DropoutMode::eHighlight: return "DROPOUT HIGHLIGHT";
    }
    return "";
}

std::string efmFilterName(const ReaderControls &reader) {
    if (!reader.efmFilterSize)
        return "";
    const int size = reader.efmFilterSize();
    return size == 0 ? "EFM FILTER OFF" : std::format("EFM FILTER {} TAPS", size);
}

std::string subtitleSlotName(const PlayerState &state, int slot) {
    if (slot < 0 || slot >= (int)state.subtitle_track_names.size())
        return "OFF";
    return state.subtitle_track_names[(size_t)slot];
}

std::string overscanName(const PlayerState &state) {
    return state.full_image ? "OVERSCAN OFF: FULL IMAGE"
                            : std::format("OVERSCAN {:g}%", state.overscan * 100);
}

} // namespace PlayerActions
