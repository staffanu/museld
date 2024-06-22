//
// Created by staffanu on 6/16/24.
//

#ifndef MUSECPP_DECODER_H
#define MUSECPP_DECODER_H

#include "AudioDefs.h"
#include "DropoutMode.h"
#include "DiscInfo.h"
#include "ResultImages.h"

class Decoder {
public:
    enum class FieldInterpolationMode {
        eNormal, eForceIntraField, eForceInterFrame
    };

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    virtual bool initialize() = 0;

    virtual bool next(bool efm_audio, AudioMode *audio_mode,
                      int *sample_count,
                      AudioFrame output_samples[MAX_AUDIO_OUTPUT_SAMPLES],
                      int *field_parity, long *last_frame_buffer_input_offset, double *input_samples_per_muse_sample,
                      std::shared_ptr<DiscInfo> *disc_info,
                      FieldInterpolationMode field_interpolation_mode,
                      bool redo_last_field, bool enable_non_linear, DropoutMode dropout_mode, bool output_yuv) = 0;

    virtual void outputBenchmarkResults() = 0;

    virtual ResultImages getResultImages() = 0;

protected:
    Decoder() = default;
};

#endif //MUSECPP_DECODER_H
