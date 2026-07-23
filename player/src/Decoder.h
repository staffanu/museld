// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DECODER_H
#define MUSECPP_DECODER_H

#include <optional>
#include "AudioDefs.h"
#include "DropoutMode.h"
#include "DiscInfo.h"
#include "ResultImages.h"

class Decoder {
public:
    enum class FieldInterpolationMode {
        eNormal, eForceIntraField, eForceInterFrame
    };

    struct SourceDimensions {
        int width;          // full image, both fields, all chroma planes laid out as in out_image
        int height;
        int field_width;    // width of a single field in pixel coordinates (Y plane)
        int field_height;   // height of a single field
    };

    struct PixelFileOffsets {
        long field_start;
        long y;
        long cr;
        long cb;
    };

    struct DecodeControls {
        bool efm_audio;
        FieldInterpolationMode field_interpolation_mode;
        bool redo_last_field;
        bool enable_non_linear;
        bool use_3d_comb;
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
        int field_parity;
        long last_frame_buffer_input_offset;
        double input_samples_per_muse_sample;
        std::shared_ptr<DiscInfo> disc_info;
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
            long buffer_file_offset, double input_samples_per_muse_sample) const = 0;

protected:
    Decoder() = default;
};

#endif //MUSECPP_DECODER_H
