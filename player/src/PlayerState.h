// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_PLAYERSTATE_H
#define MUSECPP_PLAYERSTATE_H

#include <string>
#include <utility>
#include <vector>
#include "Decoder.h"
#include "DisplayGeometry.h"

// What the listener hears from a stereo audio track: both channels, or one of
// them on both ears (bilingual discs, or the left-only analog audio on AC3
// discs).  Applies to playback only; --write always keeps the full track.
enum class AudioChannelMode { eStereo, eLeft, eRight };

struct PlayerState {
    bool paused = false;
    int paused_countdown = 0;
    Decoder::FieldInterpolationMode field_interpolation_mode = Decoder::FieldInterpolationMode::eNormal;
    bool redo_last_field = false;
    bool export_frame = false;
    bool enable_non_linear = true;
    bool use_3d_comb = true; // NTSC: temporal Y/C separation on still parts
    bool film_mode = true;   // NTSC: reverse-telecine weave on a 3:2 cadence lock
    bool enable_cursor = false;
    AudioChannelMode audio_channel_mode = AudioChannelMode::eStereo;
    Decoder::CxMode analog_cx_mode = Decoder::CxMode::eAuto;
    Decoder::BlackLevelMode black_level_mode = Decoder::BlackLevelMode::eAuto; // NTSC black setup
    bool show_disc_code = false;
    int zoom_factor = 1;
    std::pair<double, double> zoom_center{0.5, 0.5};
    AspectMode aspect_mode = AspectMode::eNormal;
    double source_aspect = 4.0 / 3.0; // the picture's intended width over height (16:9 MUSE, 4:3 NTSC)
    // The part of the decoded image on screen this frame (zoom and aspect
    // cropping applied), in decoded-image pixels: the overlays keep their
    // text inside it
    double visible_x0 = 0, visible_y0 = 0, visible_x1 = 0, visible_y1 = 0;
    // Where that part sits in the window, as fractions of the window size
    double shown_x0 = 0, shown_y0 = 0, shown_x1 = 1, shown_y1 = 1;
    std::string osd_text; // Set to update text, moved to displayed_ during display
    std::string displayed_osd_text;
    int osd_text_remaining_frames = 0;
    int field_count = 0;
    double stream_seconds = 0.0;             // playback position derived from field_count,
                                             // adjusted for interactive seeks; relative to
                                             // the --seek position (can go negative when
                                             // seeking back across it)
    double stream_seek_offset_seconds = 0.0; // accumulated left/right-arrow seek deltas
    double stream_start_seconds = 0.0;       // the initial --seek position in the input
    std::string last_cursor_string;

    // Loaded subtitle track labels, and which track each of the two display
    // slots shows (-1 = off).  The [ and ] keys cycle the slots.
    std::vector<std::string> subtitle_track_names;
    int subtitle_primary = -1;   // bottom of the frame
    int subtitle_secondary = -1; // top of the frame

    Decoder::DecodedField last_decoded{};
};

#endif //MUSECPP_PLAYERSTATE_H
