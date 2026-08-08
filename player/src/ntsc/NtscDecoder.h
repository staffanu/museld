// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCDECODER_H
#define MUSECPP_NTSCDECODER_H

#include <array>
#include <deque>
#include "musevk/TimestampStatistics.h"
#include "efm/EfmDecoder.h"
#include "efm/EfmPcmProcessor.h"
#include "musevk/CommandPool.h"
#include "NtscInputBlock.h"
#include "Decoder.h"
#include "NtscCadenceTracker.h"
#include "NtscFrame.h"
#include "NtscShaders.h"

namespace musevk {
    class TimestampQueryPool;
}
template<class InputBlock> class FrameReader;
class Logger;
class NtscShaders;

class NtscDecoder : public Decoder {
public:
    /// \param decode_all_fields If false, skips decoding of the first field of each
    /// frame individually, so next should be called 30 times per second instead of 60;
    /// useful for slow hardware.
    NtscDecoder(
            Logger &log, FrameReader<NtscInputBlock> &reader, musevk::VulkanManager &manager,
            musevk::CommandPool &command_pool, std::string const &executable_dir,
            bool decode_video, bool decode_all_fields, bool decode_audio,
            float tint_degrees, float saturation,
            musevk::TimestampQueryPool *timestamp_query_pool);
    ~NtscDecoder();
    NtscDecoder(const NtscDecoder&) = delete;
    void operator=(const NtscDecoder&) = delete;

    [[nodiscard]] bool initialize() override;

    // true if not eof.  In case of a read timeout also returns true so that we can check for key presses.
    bool next(const DecodeControls &controls, DecodedField &out) override;

    void outputBenchmarkResults() override;
    ResultImages getResultImages() override;
    SourceDimensions getSourceDimensions() const override;
    std::optional<PixelFileOffsets> computePixelFileOffsets(
            int field_x, int field_y, int field_parity,
            int64_t buffer_file_offset, double input_samples_per_muse_sample) const override;

private:
    Logger &m_log;
    FrameReader<NtscInputBlock> &m_reader;
    musevk::VulkanManager &m_manager;
    NtscShaders m_shaders;
    const bool m_decode_video;
    const bool m_decode_all_fields;
    const bool m_decode_audio;
    musevk::TimestampQueryPool *m_timestamp_query_pool; // if set we use it

    std::pair<float, float> m_eq;
    float m_rot_re; // chroma rotation/gain for the decode shader; set in the constructor
    float m_rot_im;
    NtscFrame::NoiseEstimate m_noise; // EWMA-smoothed, reader voltage units
    double m_blanking_avg;            // EWMA of the per-frame blanking level ...
    double m_blanking_sq_avg;         // ... and of its square, for the wander σ
    double m_white_avg;               // EWMA of the white flag level, -1 until first seen
    long m_white_flag_frames;         // frames whose white flag qualified
    float m_level_offset_v;           // rescale applied by the copy shader:
    float m_level_scale;              // out = (v - offset) * scale
    double m_prev_burst_phase;        // last frame's burst phase, NAN before the first
    double m_burst_coherence_avg;     // EWMA of the frame-to-frame burst phase error, -1 until seeded
    std::array<double, 256> m_noise_psd; // cumulative blank-VBI-line power spectrum
    long m_noise_psd_windows;
    vk::Semaphore m_first_stage_complete_semaphore;
    std::shared_ptr<musevk::CommandBuffer> m_reset_timestamp_query_pool_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_first_stage_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_second_stage_command_buffer;
    musevk::TimestampStatistics m_timestamp_statistics;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    NtscCadenceTracker m_cadence;
    // Which displayed frame each per-parity field buffer set in NtscShaders
    // currently holds: input starvation can skip a field decode, and a
    // cadence weave/hold must not touch a buffer one frame older than the
    // cadence assumes
    std::array<int, 2> m_field_buffer_frame_no;
    EfmDecoder m_efm_decoder;
    EfmPcmProcessor m_efm_pcm_processor;
    // Audio decoded from the newest read block, held back one frame so it
    // stays in sync with the picture (which displays one frame behind the
    // read for the 3D comb lookahead)
    std::vector<AudioFrame> m_pending_audio;
    AudioMode m_pending_audio_mode;
    // The front (index 0) is the newest received frame N+1 (the lookahead);
    // index 1 is the frame being decoded, 2 and 3 its history.
    std::deque<NtscFrame *> m_frames;
};


#endif //MUSECPP_NTSCDECODER_H
