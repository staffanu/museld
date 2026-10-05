// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOSTANDARD_H
#define MUSECPP_VIDEOSTANDARD_H

#include <vector>

// The composite video standard a laserdisc capture follows, as the SD
// (sdtv/) pipeline needs it: the line-locked sampling grid of the frame
// buffer, where the picture sits in it, the frame rate, the RF carrier, and
// the lines the Philips code rides on.  NTSC is the standard the pipeline
// was written for, and its constants are also the shaders' NTSC_* macros
// (shaders/muse/muse.h); a PAL build of the same shaders takes the PAL
// values under -DPAL_GEOMETRY, which is what shader_prefix selects.
struct VideoStandard {
    const char *name;             // "NTSC" or "PAL"
    const char *shader_prefix;    // the SPIR-V file name prefix: "ntsc" or "pal"

    // The frame buffer: total_lines rows of samples_per_line samples, line
    // locked (sample 0 of a row is the sync edge), at sampling_frequency
    int total_lines;
    int samples_per_line;
    double sampling_frequency;    // Hz
    double line_hz;
    int fps_num, fps_den;         // frames per second

    // The picture rows of a field start at frame line field_start_y
    // (1-based; field 2 at field_start_y + field2_offset) and run for
    // field_lines; the picture columns start at field_start_x and run for
    // y_buf_width (the decoded image's width)
    int field_lines;
    int field_start_y;
    int field2_offset;
    int y_buf_width;
    int field_start_x;
    int dropout_bit_words;        // (samples_per_line + 31) / 32

    // The vertical sync: field 1's broad pulses start at the top of this
    // frame line and there are this many of them at half-line spacing (field
    // 2's group starts half a line into line vsync_broad_start_line +
    // field2_offset - 1)
    int vsync_broad_start_line;
    int vsync_broad_pulses;

    // The video FM carrier: sync tip at center - deviation, white at center + deviation
    double rf_center_hz;
    double rf_deviation_hz;
    // The RF band the demodulator keeps: it must reach down to the lower
    // first-order chroma sideband (carrier - fsc: 4.5 MHz on NTSC, but only
    // 2.3-3.5 MHz on PAL, just above the EFM band) and stop below the EFM
    double rf_bandpass_low_hz, rf_bandpass_high_hz, rf_bandpass_transition_hz;
    double video_lowpass_hz;      // the demodulated video lowpass cutoff

    // The analog FM audio carriers, and the channel-select filter edges that
    // keep each one's demodulator clear of the other
    double audio_left_hz, audio_right_hz;
    double audio_channel_pass_hz, audio_channel_stop_hz;

    // Frame lines carrying the Philips code: 16-18 of each field (IEC
    // 60857), probed with a margin of one line for a frame that starts on
    // the wrong field
    std::vector<int> vbi_code_lines;
    // Blank vertical-interval lines and the two 256-sample windows in them
    // that the noise spectrum is measured on
    std::vector<int> blank_vbi_lines;
    int noise_psd_cols[2];
    // Noise statistics windows: the first of 211 picture rows per field, the
    // back porch column after the colour burst, and the active picture
    // columns of the luma histogram
    int noise_rows_start[2];
    int noise_porch_col;
    int noise_sync_col, noise_sync_len; // the sync tip window; length 0 when the tip carries a pilot (PAL)
    int luma_hist_col0, luma_hist_col1;

    bool ntsc_chroma;             // NTSC colour: the subcarrier inverts line to line and frame to frame
    double chroma_rotation_deg;   // the demodulation angle: the structural 180 plus any calibrated offset
    bool has_black_setup;         // NTSC-M's 7.5 IRE pedestal (and the automatic M/J choice)
    bool has_closed_captions;     // EIA-608 on line 21
    bool has_white_flag;          // the 100 IRE film frame flag on a VBI line
    bool has_film_cadence;        // 3:2 pulldown
    double luma_bandwidth_hz;     // for the SNR report

    // The chroma demodulation window reaches this far past the picture
    // columns (SDTV_CHROMA_TAP_HALO in the shaders)
    static constexpr int c_chroma_tap_halo = 9;

    [[nodiscard]] double framesPerSecond() const { return (double)fps_num / fps_den; }
    [[nodiscard]] double frameDurationMs() const { return 1e3 * fps_den / fps_num; }
    [[nodiscard]] int secondFieldLine(int field_line) const { return field_line + field2_offset; }

    static const VideoStandard &ntsc();
    static const VideoStandard &pal();
};

#endif //MUSECPP_VIDEOSTANDARD_H
