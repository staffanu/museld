// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_MUSEDECODER_H
#define MUSECPP_MUSEDECODER_H

#include <deque>
#include "musevk/TimestampStatistics.h"
#include "efm/EfmDecoder.h"
#include "efm/EfmPcmProcessor.h"
#include "AudioDecoder.h"
#include "Shaders.h"
#include "FrameBuffer.h"
#include "MuseAdaptiveEqualizer.h"
#include "musevk/CommandPool.h"
#include "MuseInputBlock.h"
#include "Decoder.h"

namespace musevk {
    class TimestampQueryPool;
}
class AudioDecoder;
template<class InputBlock> class FrameReader;
class Logger;
class Shaders;

class MuseDecoder : public Decoder {
public:
    /// \param decode_all_fields If false, skips decoding of the first field of each
    /// frame individually, so next should be called 30 times per second instead of 60;
    /// useful for slow hardware.
    MuseDecoder(Logger &log,
                FrameReader<MuseInputBlock> &reader,
                musevk::VulkanManager &manager,
                musevk::CommandPool &command_pool,
                std::string const &executable_dir,
                bool decode_video,
                bool decode_all_fields,
                bool decode_audio,
                MuseAdaptiveEqualizer::Mode eq_mode,
                float eq_alpha,
                musevk::TimestampQueryPool *timestamp_query_pool);

    // Interactive controls (key bindings in InputController).
    MuseAdaptiveEqualizer::Mode cycleEqMode() { return m_equalizer.cycleMode(); }
    void resetEqTaps() { m_equalizer.resetTaps(); }
    ~MuseDecoder();
    MuseDecoder(const MuseDecoder&) = delete;
    void operator=(const MuseDecoder&) = delete;

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
    FrameReader<MuseInputBlock> &m_reader;
    musevk::VulkanManager &m_manager;
    Shaders m_shaders;
    const bool m_decode_video;
    const bool m_decode_all_fields;
    const bool m_decode_audio;
    musevk::TimestampQueryPool *m_timestamp_query_pool; // if set we use it

    std::pair<float, float> m_rescale;
    vk::Semaphore m_first_stage_complete_semaphore;
    std::shared_ptr<musevk::CommandBuffer> m_reset_timestamp_query_pool_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_first_stage_command_buffer;
    std::shared_ptr<musevk::CommandBuffer> m_second_stage_command_buffer;
    musevk::TimestampStatistics m_timestamp_statistics;
    int m_frame_no;
    int m_field_index; // 0 if a new frame needs to be read, 1 when we should process the second field
    long m_total_elapsed_time_us;
    AudioDecoder m_audio_decoder;
    EfmDecoder m_efm_decoder;
    EfmPcmProcessor m_efm_pcm_processor;
    MuseAdaptiveEqualizer m_equalizer;
    std::deque<FrameBuffer *> m_frame_buffers; // The front (index 0) is the newest received frame
};


#endif //MUSECPP_MUSEDECODER_H
