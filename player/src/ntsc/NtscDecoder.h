//
// Created by staffanu on 5/22/23.
//

#ifndef MUSECPP_NTSCDECODER_H
#define MUSECPP_NTSCDECODER_H

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
            musevk::TimestampQueryPool *timestamp_query_pool);
    ~NtscDecoder();
    NtscDecoder(const NtscDecoder&) = delete;
    void operator=(const NtscDecoder&) = delete;

    [[nodiscard]] bool initialize() override;

    // true if not eof.  In case of a read timeout also returns true so that we can check for key presses.
    bool next(bool efm_audio, AudioMode *audio_mode,
              int *sample_count,
              AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES],
              int *field_parity, long *last_frame_buffer_input_offset, double *input_samples_per_muse_sample,
              std::shared_ptr<DiscInfo> *disc_code,
              FieldInterpolationMode field_interpolation_mode,
              bool redo_last_field, bool enable_non_linear, DropoutMode dropout_mode, bool output_yuv) override;

    void outputBenchmarkResults() override;
    ResultImages getResultImages() override;

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
