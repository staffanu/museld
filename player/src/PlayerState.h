// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_PLAYERSTATE_H
#define MUSECPP_PLAYERSTATE_H

#include <string>
#include <utility>
#include <vector>
#include "Decoder.h"

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
    bool show_disc_code = false;
    int zoom_factor = 1;
    std::pair<double, double> zoom_center{0.5, 0.5};
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
