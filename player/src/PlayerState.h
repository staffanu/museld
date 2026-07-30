// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_PLAYERSTATE_H
#define MUSECPP_PLAYERSTATE_H

#include <string>
#include <utility>
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
    std::string last_cursor_string;

    Decoder::DecodedField last_decoded{};
};

#endif //MUSECPP_PLAYERSTATE_H
