// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DECODER_H
#define MUSECPP_DECODER_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "AudioDefs.h"
#include "DropoutMode.h"
#include "DiscInfo.h"
#include "ResultImages.h"

class Decoder {
public:
    enum class FieldInterpolationMode {
        eNormal, eForceIntraField, eForceInterFrame
    };

    // CX noise reduction for the NTSC analog audio: follow the VBI flag, or
    // force it off/on (for discs without a readable flag, or for A/B listening)
    enum class CxMode {
        eAuto, eOff, eOn
    };

    struct SourceDimensions {
        int width;          // full image, both fields, all chroma planes laid out as in out_image
        int height;
        int field_width;    // width of a single field in pixel coordinates (Y plane)
        int field_height;   // height of a single field
    };

    struct PixelFileOffsets {
        int64_t field_start;
        int64_t y;
        // Chroma sample offsets. Unset for composite formats (NTSC), whose file
        // carries no separate Cr/Cb samples — there Y is the only real offset.
        std::optional<int64_t> cr;
        std::optional<int64_t> cb;
    };

    struct DecodeControls {
        AudioTrack audio_track;
        CxMode analog_cx;
        FieldInterpolationMode field_interpolation_mode;
        bool redo_last_field;
        bool enable_non_linear;
        bool use_3d_comb;
        bool film_mode; // NTSC: weave by the film cadence when a 3:2 pulldown lock holds
        DropoutMode dropout_mode;
        bool output_yuv;
    };

    struct DecodedField {
        // False when next() returned true without producing a field, which it
        // does on a read timeout so that the caller stays responsive. Anything
        // counting fields has to look at this: the timeouts are frequent enough
        // to shift a stream position by several fields.
        bool decoded;
        AudioMode audio_mode;
        int audio_sample_count;
        AudioFrame audio_samples[MAX_AUDIO_OUTPUT_SAMPLES];
        // The raw AC3-RF sync frames behind this frame's MODE_AC3 audio, so
        // the file writer can mux the original bitstream instead of PCM;
        // empty for every other mode
        std::vector<std::array<uint8_t, 1536>> ac3_frames;
        int field_parity;
        int64_t last_frame_buffer_input_offset;
        double input_samples_per_muse_sample;
        std::shared_ptr<DiscInfo> disc_info;
        // EIA-608 closed caption byte pair from NTSC line 21 (field 1, parity
        // bits intact).  Set at most once per frame read, so the consumer sees
        // each pair exactly once; always unset for decoders without captions
        // (MUSE).
        std::optional<std::pair<uint8_t, uint8_t>> cc_bytes;
        // Film mode status for the disc info overlay; empty for decoders
        // without film mode (MUSE).  The detail part changes per field, so
        // the overlay only appends it while paused, where it can be read.
        std::string film_status;
        std::string film_status_detail;
    };

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    virtual ~Decoder() = default;

    virtual bool initialize() = 0;

    // true if not eof.  In case of a read timeout also returns true so that we can check for key presses.
    virtual bool next(const DecodeControls &controls, DecodedField &out) = 0;

    virtual void outputBenchmarkResults() = 0;

    virtual ResultImages getResultImages() = 0;

    virtual SourceDimensions getSourceDimensions() const = 0;

    // For the cursor-coordinate overlay's file-offset readout. MUSE returns a real
    // offset; NTSC returns std::nullopt and the overlay omits that line.
    virtual std::optional<PixelFileOffsets> computePixelFileOffsets(
            int field_x, int field_y, int field_parity,
            int64_t buffer_file_offset, double input_samples_per_muse_sample) const = 0;

protected:
    Decoder() = default;
};

#endif //MUSECPP_DECODER_H
