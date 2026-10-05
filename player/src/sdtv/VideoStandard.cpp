// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "VideoStandard.h"

// The geometry values here are the C++ side of the shaders' NTSC_* macros
// (shaders/muse/muse.h, which holds both standards' sets): the two must agree.

const VideoStandard &VideoStandard::ntsc() {
    // 4 fsc = 315/88 MHz * 4 = 14.3182 MHz = 910 * 525 * 30 / 1.001.  Field
    // 1's picture is lines 22-261, field 2's 285-524; the picture columns
    // start 9.0 us after the sync edge and the 764 columns span 53.4 us
    static const VideoStandard s{
        .name = "NTSC",
        .shader_prefix = "ntsc",
        .total_lines = 525,
        .samples_per_line = 910,
        .sampling_frequency = 315.0 / 88 * 1e6 * 4,
        .line_hz = 315.0 / 88 * 1e6 * 4 / 910,
        .fps_num = 30000, .fps_den = 1001,
        .field_lines = 240,
        .field_start_y = 22,
        .field2_offset = 263,
        .y_buf_width = 764,
        .field_start_x = 129,
        .dropout_bit_words = (910 + 31) / 32,
        // six broad pulses from the top of line 4 to the middle of line 6
        .vsync_broad_start_line = 4,
        .vsync_broad_pulses = 6,
        // sync tip 7.6 MHz, blanking 8.1, white 9.3
        .rf_center_hz = 8.5e6,
        .rf_deviation_hz = 0.85e6,
        .rf_bandpass_low_hz = 3.5e6, .rf_bandpass_high_hz = 13.5e6, .rf_bandpass_transition_hz = 1.5e6,
        .video_lowpass_hz = 5e6,
        .audio_left_hz = 2.3011e6, .audio_right_hz = 2.8125e6,
        // field 2 starts at line 264 in the frame buffer, so its code lines
        // are 279-281; 278 covers a frame started on the wrong field
        .vbi_code_lines = {16, 17, 18, 278, 279, 280, 281},
        .blank_vbi_lines = {8, 10, 11, 12, 13, 14, 270, 272, 273, 274, 275, 276},
        .noise_psd_cols = {150, 425},
        .noise_rows_start = {40, 303},
        .noise_porch_col = 113,     // after the burst (ends ~112), before the picture at 129
        .noise_sync_col = 8, .noise_sync_len = 48,
        .luma_hist_col0 = 152, .luma_hist_col1 = 872,
        .ntsc_chroma = true,
        .has_black_setup = true,
        .has_closed_captions = true,
        .has_white_flag = true,
        .has_film_cadence = true,
        .luma_bandwidth_hz = 4.2e6,
    };
    return s;
}

const VideoStandard &VideoStandard::pal() {
    // Line locked at 1135 samples per line: 17.734375 MHz, 100 Hz below
    // 4 fsc (exact 4 fsc is 1135.0064 samples per line because of the 25 Hz
    // offset), so the subcarrier walks 0.576 degrees per line against the
    // grid -- one cycle per frame -- and a colour decoder must take its
    // phase from each line's burst.  See docs/pal-playback-plan.md.
    static const VideoStandard s{
        .name = "PAL",
        .shader_prefix = "pal",
        .total_lines = 625,
        .samples_per_line = 1135,
        .sampling_frequency = 1135 * 15625.0,
        .line_hz = 15625.0,
        .fps_num = 25, .fps_den = 1,
        // field 1's picture is lines 23-310, field 2's 336-623; the picture
        // columns start 10.15 us after the sync edge (blanking ends at 10.5)
        // and the 944 columns span 53.2 us
        .field_lines = 288,
        .field_start_y = 23,
        .field2_offset = 313,
        .y_buf_width = 944,
        .field_start_x = 180,
        .dropout_bit_words = (1135 + 31) / 32,
        // PAL numbers its lines from the vertical sync: five broad pulses
        // from the top of line 1 to the middle of line 3
        .vsync_broad_start_line = 1,
        .vsync_broad_pulses = 5,
        // sync tip 6.76 MHz, blanking 7.1, white 7.9
        .rf_center_hz = 7.33e6,
        .rf_deviation_hz = 0.57e6,
        // The same band as NTSC.  ld-decode uses 2.3-14 MHz for PAL, which
        // admits the lower chroma sideband (carrier - 4.43 MHz = 2.3-3.5 MHz)
        // -- and the lower sideband of the 3.75 MHz pilot burst on the sync
        // tip (IEC 60856 9.1.2: 60 IRE peak-to-peak, 0.5-4.1 us into the
        // pulse), at 3.0 MHz when the carrier sits at sync tip.  Admitted
        // through a sharp FIR (1 MHz transition, 160 taps = 4 us at 40 MHz)
        // the pilot's energy rings into the back porch and read as 6-7 IRE
        // of blanking noise instead of 4.2.  Cutting the lower chroma
        // sideband costs the chroma 3 dB of SNR and half its amplitude,
        // which the burst-referenced gain restores; the phase survives.
        .rf_bandpass_low_hz = 3.5e6, .rf_bandpass_high_hz = 13.5e6, .rf_bandpass_transition_hz = 1.5e6,
        .video_lowpass_hz = 5.8e6,
        .audio_left_hz = 43.75 * 15625.0, .audio_right_hz = 68.25 * 15625.0,
        // field 2's lines 16-18 are frame lines 329-331 (its first line, 313,
        // is a half line); 328 covers a frame started on the wrong field
        .vbi_code_lines = {16, 17, 18, 328, 329, 330, 331},
        .blank_vbi_lines = {7, 8, 9, 10, 11, 12, 320, 321, 322, 323, 324, 325},
        .noise_psd_cols = {190, 600},
        .noise_rows_start = {40, 353},
        .noise_porch_col = 142,     // after the burst (ends ~139), before the picture at 180
        // the 3.75 MHz pilot burst occupies the sync tip from 0.5 to 4.1 us
        // (IEC 60856 9.1.2) -- up to the rising edge once filtered -- so the
        // tip noise is not measurable
        .noise_sync_col = 0, .noise_sync_len = 0,
        .luma_hist_col0 = 196, .luma_hist_col1 = 1100,
        .ntsc_chroma = false,
        .has_black_setup = false,
        .has_closed_captions = false,
        .has_white_flag = false,
        .has_film_cadence = false,
        .luma_bandwidth_hz = 5.0e6,
    };
    return s;
}
