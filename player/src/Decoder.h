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

    // NTSC black setup: 7.5 IRE above blanking (NTSC-M, US discs) or at
    // blanking (NTSC-J); auto reads it off the picture's own dark end
    enum class BlackLevelMode {
        eAuto, eM, eJ
    };

    // SDTV temporal noise reduction: a motion-gated first-order recursion on
    // the decoded Y, U and V, off or with a feedback of 0.5 (low) or 0.75
    // (high) per field
    enum class TemporalNr {
        eOff, eLow, eHigh
    };
    static float temporalNrFeedback(TemporalNr nr) {
        return nr == TemporalNr::eLow ? 0.5f : nr == TemporalNr::eHigh ? 0.75f : 0.0f;
    }

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
        // NTSC: where the disc puts black; black is placed at output zero
        // and the picture above it (chroma included) rescaled to fill the range
        BlackLevelMode black_level;
        // Only the disc metadata is wanted (the chapter search probing the
        // input): the video stays at the previous frame and no audio is
        // decoded, which makes a probe cost little more than the seek
        bool metadata_only = false;
        TemporalNr temporal_nr = TemporalNr::eOff; // SDTV only
        // SDTV: interpolate the moving parts' missing lines along the local
        // edge direction (off: from the two vertical neighbours only)
        bool edge_directed = true;
        // SDTV: audio tracks decoded besides audio_track, for a --write file
        // with a stream per track (DecodedField::track_audio); the reader
        // must demodulate them too (FrameReader::setExtraAudioTracks)
        std::vector<AudioTrack> extra_tracks;
    };

    // One audio track's output of a field, for the file writer
    struct TrackAudio {
        AudioTrack track;
        AudioMode mode;
        std::vector<AudioFrame> samples;
        std::vector<std::array<uint8_t, 1536>> ac3_frames; // MODE_AC3: the originals
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
        // Every track decoded this field, the played one (audio_samples
        // above, repeated here) and DecodeControls::extra_tracks, for a
        // --write file with a stream per track; empty for decoders with one
        // track (MUSE), whose writer takes audio_samples
        std::vector<TrackAudio> track_audio;
        int field_parity;
        int64_t last_frame_buffer_input_offset;
        double input_samples_per_muse_sample;
        std::shared_ptr<DiscInfo> disc_info;
        // The input sample offset of the frame disc_info was read from
        // (the two decoders attach the info of different frames relative to
        // the displayed one), and the seek generation that frame came from:
        // readers count their seeks and stamp every frame with the count,
        // so a frame still in flight from before a seek can be told apart
        // from the first one after it
        int64_t disc_info_input_offset;
        uint32_t seek_generation;
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
        // Black level setting and the measured picture black, for the same
        // overlay; empty for MUSE
        std::string level_status;
        // Whether the EFM decoder is locked to frame syncs, on the fields
        // where the EFM track was decoded (SDTV): the automatic fallback from
        // a silent analog track checks that the EFM track exists at all
        std::optional<bool> efm_locked;
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
