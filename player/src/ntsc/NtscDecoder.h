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
            long buffer_file_offset, double input_samples_per_muse_sample) const override;

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
    EfmDecoder m_efm_decoder;
    EfmPcmProcessor m_efm_pcm_processor;
    std::deque<NtscFrame *> m_frames; // The front (index 0) is the newest received frame; we keep three frames.
};


#endif //MUSECPP_NTSCDECODER_H
