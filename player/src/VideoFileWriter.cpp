//
// Created by staffanu on 3/7/24.
//

#include <algorithm>
#include <climits>
#include "VideoFileWriter.h"
#include "musevk/VulkanBuffer.h"

// Audio/video sync thresholds.  The audio pts is a contiguous sample count, so
// the container cannot express gaps -- samples lost to stream errors would shift
// all later audio earlier.  Offsets within the dead band are left alone (audio
// arrives in per-frame chunks, so the measured offset jitters by up to a chunk);
// deficits up to the stretch limit are concealed by interpolation, larger gaps
// are filled with silence.
static constexpr int SYNC_DEAD_BAND_MS = 25;
static constexpr int SYNC_MAX_STRETCH_MS = 100;

VideoFileWriter::PresetSpec VideoFileWriter::presetSpec(VideoWriterPreset preset) {
    switch (preset) {
        case VideoWriterPreset::eStandard:
            return {"mp4", "libx264", "aac", AV_PIX_FMT_YUV420P};
        case VideoWriterPreset::eArchival:
            return {"matroska", "ffv1", "pcm_s16le", AV_PIX_FMT_YUV420P16LE};
    }
    throw std::runtime_error("Unknown video writer preset");
}

static int audioModeSampleRate(AudioMode mode) {
    switch (mode) {
        case MODE_A:   return 32000;
        case MODE_B:   return 48000;
        case MODE_EFM: return 44100;
        default:       return 0;
    }
}

bool VideoFileWriter::init() {
    const PresetSpec spec = presetSpec(m_preset);

    // The muxer is chosen by the preset, not deduced from the filename extension
    avformat_alloc_output_context2(&m_format_context, nullptr, spec.format_name, m_filename.c_str());
    if (!m_format_context) {
        m_log.error(eOutput, std::format("Could not allocate output context for format '{}'", spec.format_name));
        return false;
    }

    initVideo(spec);
    m_have_video = true;
    m_encode_video = true;

    initAudio(spec);
    m_have_audio = true;
    m_encode_audio = true;

    av_dump_format(m_format_context, 0, m_filename.c_str(), 1);

    // open the output file, if needed
    if (!(m_format_context->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&m_format_context->pb, m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
            throw std::runtime_error(std::format("Could not open '{}': {}", m_filename, av_err2string(ret)));
    }

    // Write the stream header, if any
    int ret = avformat_write_header(m_format_context, nullptr);
    if (ret < 0)
        throw std::runtime_error(std::format("Error occurred when opening output file: {}", av_err2string(ret)));

    return true;
}

void VideoFileWriter::addVideoFrameWithAudio(
        std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
        std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V,
        AudioMode audio_mode, int number_of_samples, AudioFrame *audio_samples) {

    //av_compare_ts(m_video_stream.next_pts, m_video_stream.codec_context->time_base,
    //              m_audio_stream.next_pts, m_audio_stream.codec_context->time_base)

    if (m_encode_video) {
        assert(image_Y->size().x_size == m_video_stream.codec_context->width);
        assert(image_Y->size().y_size == m_video_stream.codec_context->height);
        auto frame = makeVideoFrame(&m_video_stream, image_Y, image_U, image_V);
        m_encode_video = !writeFrame(m_video_stream.codec_context, m_video_stream.stream, frame, m_video_stream.tmp_pkt);
    }

    if (number_of_samples != 0 && audio_mode != MODE_UNKNOWN && m_encode_audio) {
        initResampler(audio_mode);

        // Compare the samples delivered so far against the video clock and
        // correct before writing, so decoding errors cannot desync the streams
        const int in_rate = audioModeSampleRate(audio_mode);
        const int64_t scheduled =
                (m_video_stream.next_pts - m_audio_anchor_frame) * in_rate / m_video_frame_rate;
        const int64_t deficit = scheduled - (m_audio_in_samples + number_of_samples);

        int skip = 0;
        if (deficit > in_rate * SYNC_MAX_STRETCH_MS / 1000) {
            insertSilence(deficit, in_rate);
        } else if (deficit > in_rate * SYNC_DEAD_BAND_MS / 1000) {
            insertInterpolated(deficit, in_rate, audio_samples[0]);
        } else if (deficit < -in_rate * SYNC_DEAD_BAND_MS / 1000) {
            skip = (int) std::min<int64_t>(-deficit, number_of_samples);
            m_log.warn(eOutput, std::format("Audio {} ms ahead of the video clock; dropping {} samples",
                                            -deficit * 1000 / in_rate, skip));
        }

        auto *q = (int16_t *) m_audio_tmp_buffer;
        for (int i = skip; i < number_of_samples; i++)
            for (int j = 0; j < m_audio_stream.codec_context->ch_layout.nb_channels; j++)
                *q++ = audio_samples[i].samples[j];

        const uint8_t *in[8] = {m_audio_tmp_buffer};
        convertAndWriteAudio(in, number_of_samples - skip);
        m_audio_in_samples += number_of_samples - skip;
        m_last_sample[0] = audio_samples[number_of_samples - 1].samples[0];
        m_last_sample[1] = audio_samples[number_of_samples - 1].samples[1];
    }
}

// Feed samples into the resampler and hand completed frames to the encoder.
// Called with in == nullptr (and in_samples == 0) to flush the resampler at end of stream.
void VideoFileWriter::convertAndWriteAudio(const uint8_t **in, int in_samples) {
    int ret = av_frame_make_writable(m_audio_stream.frame);
    if (ret < 0)
        throw std::runtime_error("Failed to make audio frame writable");

    int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)m_audio_stream.frame->format);

    bool filled_frame;
    do {
        uint8_t *out[8];
        if (av_sample_fmt_is_planar((AVSampleFormat)m_audio_stream.frame->format)) {
            for (int i = 0; i < m_audio_stream.codec_context->ch_layout.nb_channels; i++)
                out[i] = {m_audio_stream.frame->data[i] + m_samples_in_frame * bytes_per_sample};
        } else {
            out[0] = {m_audio_stream.frame->data[0]
                      + m_samples_in_frame * bytes_per_sample * m_audio_stream.codec_context->ch_layout.nb_channels};
        }

        ret = swr_convert(m_swr_ctx, out,
                          m_audio_stream.frame->nb_samples - m_samples_in_frame,
                          in, in_samples);
        in_samples = 0;
        if (ret < 0)
            throw std::runtime_error("Error while converting audio");

        m_samples_in_frame += ret;

        assert (m_samples_in_frame <= m_audio_stream.frame->nb_samples);
        filled_frame = m_samples_in_frame == m_audio_stream.frame->nb_samples;
        if (filled_frame) {

            m_encode_audio = !writeFrame(m_audio_stream.codec_context, m_audio_stream.stream, m_audio_stream.frame,
                                         m_audio_stream.tmp_pkt);

            assert(av_frame_is_writable(m_audio_stream.frame)); // it seems like audio frames are always writable after making writable the first time?

            m_audio_stream.frame->pts = m_audio_stream.next_pts;
            m_audio_stream.next_pts += m_audio_stream.frame->nb_samples;
            m_samples_in_frame = 0;
        }
    } while (filled_frame && m_encode_audio);
}

// Fill a gap of `count` input samples with silence, fading out from the last
// written sample so the transition into the gap does not pop
void VideoFileWriter::insertSilence(int64_t count, int in_rate) {
    m_log.warn(eOutput, std::format("Audio {} ms behind the video clock; inserting silence", count * 1000 / in_rate));

    const int channels = m_audio_stream.codec_context->ch_layout.nb_channels;
    const int capacity = (int) (sizeof(m_audio_tmp_buffer) / (channels * sizeof(int16_t)));
    const int64_t fade = std::min<int64_t>(64, count);

    int64_t generated = 0;
    while (generated < count) {
        int n = (int) std::min<int64_t>(count - generated, capacity);
        auto *q = (int16_t *) m_audio_tmp_buffer;
        for (int i = 0; i < n; i++, generated++)
            for (int j = 0; j < channels; j++)
                *q++ = generated < fade ? (int16_t) (m_last_sample[j] * (fade - generated) / (fade + 1)) : 0;
        const uint8_t *in[8] = {m_audio_tmp_buffer};
        convertAndWriteAudio(in, n);
    }
    m_last_sample[0] = m_last_sample[1] = 0;
    m_audio_in_samples += count;
}

// Conceal a small deficit by stretching: insert `count` samples interpolated
// between the last written sample and the first sample about to be written
void VideoFileWriter::insertInterpolated(int64_t count, int in_rate, const AudioFrame &next) {
    m_log.info(eOutput, std::format("Audio {} ms behind the video clock; inserting {} interpolated samples",
                                    count * 1000 / in_rate, count));

    const int channels = m_audio_stream.codec_context->ch_layout.nb_channels;
    count = std::min<int64_t>(count, (int64_t) (sizeof(m_audio_tmp_buffer) / (channels * sizeof(int16_t))));

    auto *q = (int16_t *) m_audio_tmp_buffer;
    for (int64_t i = 1; i <= count; i++)
        for (int j = 0; j < channels; j++)
            *q++ = (int16_t) (m_last_sample[j] + (next.samples[j] - m_last_sample[j]) * i / (count + 1));
    const uint8_t *in[8] = {m_audio_tmp_buffer};
    convertAndWriteAudio(in, (int) count);
    m_audio_in_samples += count;
}

void VideoFileWriter::cleanup() {
    // Pad the audio track with silence up to the video clock, so the streams
    // end together even if audio was lost at the end of the recording
    if (m_encode_audio && m_swr_ctx) {
        int in_rate = audioModeSampleRate(m_audio_input_mode);
        int64_t deficit = (m_video_stream.next_pts - m_audio_anchor_frame) * in_rate / m_video_frame_rate
                          - m_audio_in_samples;
        if (deficit > in_rate * SYNC_DEAD_BAND_MS / 1000)
            insertSilence(deficit, in_rate);
    }

    // Flush pending audio: drain the resampler, then send the final (shorter) frame
    if (m_encode_audio && m_swr_ctx) {
        convertAndWriteAudio(nullptr, 0);
        if (m_samples_in_frame > 0 && m_encode_audio) {
            m_audio_stream.frame->nb_samples = m_samples_in_frame;
            m_encode_audio = !writeFrame(m_audio_stream.codec_context, m_audio_stream.stream, m_audio_stream.frame,
                                         m_audio_stream.tmp_pkt);
        }
    }

    // Drain the encoders; without this, frames still buffered inside the
    // encoder (e.g. x264 lookahead and B frames) are lost
    if (m_encode_video)
        writeFrame(m_video_stream.codec_context, m_video_stream.stream, nullptr, m_video_stream.tmp_pkt);
    if (m_encode_audio)
        writeFrame(m_audio_stream.codec_context, m_audio_stream.stream, nullptr, m_audio_stream.tmp_pkt);

    av_write_trailer(m_format_context);

    if (m_have_video)
        close_stream(m_format_context, &m_video_stream);
    if (m_have_audio)
        close_stream(m_format_context, &m_audio_stream);

    swr_free(&m_swr_ctx);

    if (!(m_format_context->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m_format_context->pb); // Close the output file

    avformat_free_context(m_format_context);
}

void VideoFileWriter::initStream(OutputStream *ost, const char *codec_name) {
    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt)
        throw std::runtime_error("Could not allocate AVPacket");

    ost->stream = avformat_new_stream(m_format_context, nullptr);
    if (!ost->stream)
        throw std::runtime_error("Could not allocate stream");
    ost->stream->id = m_format_context->nb_streams - 1;

    // Look up the specific encoder by name (several encoders can implement one
    // codec id, e.g. libx264 vs h264_videotoolbox)
    ost->codec = avcodec_find_encoder_by_name(codec_name);
    if (!ost->codec)
        throw std::runtime_error(std::format("Could not find encoder '{}'", codec_name));

    if (avformat_query_codec(m_format_context->oformat, ost->codec->id, FF_COMPLIANCE_NORMAL) != 1)
        throw std::runtime_error(std::format("Codec {} cannot be stored in format {}",
                                             codec_name, m_format_context->oformat->name));

    AVCodecContext *c = avcodec_alloc_context3(ost->codec);
    if (!c)
        throw std::runtime_error("Could not alloc an encoding context");
    ost->codec_context = c;

    // Some formats want stream headers to be separate
    if (m_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

void VideoFileWriter::initVideo(const PresetSpec &spec) {
    initStream(&m_video_stream, spec.video_codec);

    AVCodecContext *c = m_video_stream.codec_context;
    c->width = m_video_width;   // Resolution must be a multiple of two.
    c->height = m_video_height;
    m_video_stream.stream->time_base = (AVRational) {1, m_video_frame_rate};
    c->time_base = m_video_stream.stream->time_base;
    c->pix_fmt = spec.pix_fmt;
    c->thread_count = 0; // auto

    // Sample (pixel) aspect ratio derived from the intended display aspect ratio
    AVRational sar;
    av_reduce(&sar.num, &sar.den,
              (int64_t)m_display_aspect.num * m_video_height,
              (int64_t)m_display_aspect.den * m_video_width, INT_MAX);
    c->sample_aspect_ratio = sar;
    m_video_stream.stream->sample_aspect_ratio = sar;

    // Color description.  These are pure metadata: they tell the player how to
    // interpret the samples; no conversion is performed by the encoder.
    switch (m_color_standard) {
        case VideoColorStandard::eBt709:
            // MUSE colorimetry is nominally SMPTE 240M, but 240M and BT.709
            // primaries are near identical and player support for the BT.709
            // tags is far better.  Note that the shader encodes Y with a pure
            // 2.2 power function; BT.709 is the closest widely supported tag.
            c->colorspace = AVCOL_SPC_BT709;
            c->color_primaries = AVCOL_PRI_BT709;
            c->color_trc = AVCOL_TRC_BT709;
            break;
        case VideoColorStandard::eSmpte170m:
            c->colorspace = AVCOL_SPC_SMPTE170M;
            c->color_primaries = AVCOL_PRI_SMPTE170M;
            c->color_trc = AVCOL_TRC_SMPTE170M;
            break;
    }
    c->color_range = AVCOL_RANGE_JPEG; // the shaders output full swing 0..255 (<<8), not studio 16..235
    c->chroma_sample_location = AVCHROMA_LOC_TOPLEFT; // the shaders subsample by taking the even row/col sample

    switch (m_preset) {
        case VideoWriterPreset::eStandard:
            // Constant quality (CRF) gives better quality per bit than a fixed
            // bit rate; these are libx264 private options
            av_opt_set(c->priv_data, "crf", "18", 0);
            av_opt_set(c->priv_data, "preset", "medium", 0);
            break;
        case VideoWriterPreset::eArchival:
            c->level = 3;     // FFV1 version 3: multithreaded, per-slice CRCs
            c->gop_size = 1;  // intra only
            av_opt_set_int(c->priv_data, "slicecrc", 1, 0);
            break;
    }

    int ret = avcodec_open2(c, m_video_stream.codec, nullptr);
    if (ret < 0)
        throw std::runtime_error(std::format("Could not open video codec: {}", av_err2string(ret)));

    m_video_stream.frame = av_frame_alloc();
    if (!m_video_stream.frame)
        throw std::runtime_error("Could not allocate video frame");
    m_video_stream.frame->format = c->pix_fmt;
    m_video_stream.frame->width = c->width;
    m_video_stream.frame->height = c->height;
    m_video_stream.frame->sample_aspect_ratio = sar;
    m_video_stream.frame->colorspace = c->colorspace;
    m_video_stream.frame->color_primaries = c->color_primaries;
    m_video_stream.frame->color_trc = c->color_trc;
    m_video_stream.frame->color_range = c->color_range;
    m_video_stream.frame->chroma_location = c->chroma_sample_location;
    ret = av_frame_get_buffer(m_video_stream.frame, 0);
    if (ret < 0)
        throw std::runtime_error("Could not allocate video frame buffer");

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(m_video_stream.stream->codecpar, c);
    if (ret < 0)
        throw std::runtime_error("Could not copy the stream parameters");
}

void VideoFileWriter::initAudio(const PresetSpec &spec) {
    initStream(&m_audio_stream, spec.audio_codec);

    AVCodecContext *c = m_audio_stream.codec_context;

    const AVSampleFormat *sample_formats;
    int no_sample_formats;
    int ret = avcodec_get_supported_config(
        c, m_audio_stream.codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&sample_formats, &no_sample_formats);
    if (ret < 0)
        throw std::runtime_error("Could not get sample formats");
    c->sample_fmt = no_sample_formats > 0 ? sample_formats[0] : AV_SAMPLE_FMT_FLTP;

    const int *samplerates;
    int no_samplerates;
    ret = avcodec_get_supported_config(
        c, m_audio_stream.codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void **)&samplerates, &no_samplerates);
    if (ret < 0)
        throw std::runtime_error("Could not get sample rates");

    c->bit_rate = 192000; // only meaningful for lossy codecs; ignored by PCM
    c->sample_rate = 44100;
    if (no_samplerates > 0) // ERROR on zero?
        c->sample_rate = samplerates[0];
    for (int i = 0; i < no_samplerates; i++) {
        if (samplerates[i] == 44100)
            c->sample_rate = 44100;
    }

    // MODE_A carries 4 channels; we currently write the first two only
    auto layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&c->ch_layout, &layout);
    m_audio_stream.stream->time_base = (AVRational) {1, c->sample_rate};

    ret = avcodec_open2(c, m_audio_stream.codec, nullptr);
    if (ret < 0)
        throw std::runtime_error(std::format("Could not open audio codec: {}", av_err2string(ret)));

    int nb_samples = c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE ? 1024 : c->frame_size;

    m_audio_stream.frame = av_frame_alloc();
    if (!m_audio_stream.frame)
        throw std::runtime_error("Error allocating an audio frame");
    m_audio_stream.frame->format = c->sample_fmt;
    av_channel_layout_copy(&m_audio_stream.frame->ch_layout, &c->ch_layout);
    m_audio_stream.frame->sample_rate = c->sample_rate;
    m_audio_stream.frame->nb_samples = nb_samples;
    m_audio_stream.frame->pts = 0;
    if (nb_samples) {
        if (av_frame_get_buffer(m_audio_stream.frame, 0) < 0)
            throw std::runtime_error("Error allocating an audio buffer");
    }

    // copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(m_audio_stream.stream->codecpar, c);
    if (ret < 0)
        throw std::runtime_error("Could not copy the stream parameters");

    // The resampler is created lazily (initResampler) once the first audio
    // arrives, since the input sample rate depends on the audio mode
}

void VideoFileWriter::initResampler(AudioMode mode) {
    if (mode == m_audio_input_mode)
        return;

    AVCodecContext *c = m_audio_stream.codec_context;
    int in_sample_rate = audioModeSampleRate(mode);

    if (m_swr_ctx) {
        m_log.warn(eOutput, std::format("Audio mode changed mid-recording (input now {} Hz); "
                                        "samples pending in the resampler are dropped", in_sample_rate));
        swr_free(&m_swr_ctx);
    }

    m_swr_ctx = swr_alloc();
    if (!m_swr_ctx)
        throw std::runtime_error("Could not allocate resampler context");

    // No asserts here: side effects in assert() disappear in NDEBUG builds
    int ret = av_opt_set_chlayout(m_swr_ctx, "in_chlayout", &c->ch_layout, 0);
    ret |= av_opt_set_int(m_swr_ctx, "in_sample_rate", in_sample_rate, 0);
    ret |= av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    ret |= av_opt_set_chlayout(m_swr_ctx, "out_chlayout", &c->ch_layout, 0);
    ret |= av_opt_set_int(m_swr_ctx, "out_sample_rate", c->sample_rate, 0);
    ret |= av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt", c->sample_fmt, 0);
    if (ret != 0)
        throw std::runtime_error("Failed to configure the resampling context");

    ret = swr_init(m_swr_ctx);
    if (ret < 0)
        throw std::runtime_error("Failed to initialize the resampling context");

    // Anchor the sync schedule.  At a mode change the sample counts are in
    // different rates, so restart it at the current frame (the video frame was
    // already written, hence the -1).  On the very first init the anchor stays
    // at frame 0, so audio that starts late is padded with leading silence.
    if (m_audio_input_mode != MODE_UNKNOWN)
        m_audio_anchor_frame = m_video_stream.next_pts - 1;
    m_audio_in_samples = 0;

    m_audio_input_mode = mode;
}

void VideoFileWriter::close_stream(AVFormatContext *oc, OutputStream *ost) {
    avcodec_free_context(&ost->codec_context);
    av_frame_free(&ost->frame);
    av_packet_free(&ost->tmp_pkt);
}

AVFrame *VideoFileWriter::makeVideoFrame(OutputStream *ost, std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
                                         std::shared_ptr<musevk::VulkanBuffer> const &image_U,
                                         std::shared_ptr<musevk::VulkanBuffer> const &image_V) {
    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
        throw std::runtime_error("Failed to make video frame writable");

    auto sizeY = image_Y->size();
    auto sizeU = image_U->size();
    assert(image_V->size() == sizeU);
    assert(sizeY.x_size == sizeU.x_size * 2);
    assert(sizeY.y_size == sizeU.y_size * 2);

    // Plane order: data[0] = Y, data[1] = Cb (out_U carries B-Y), data[2] = Cr (out_V carries R-Y)
    if (ost->codec_context->pix_fmt == AV_PIX_FMT_YUV420P16LE) {
        for (int y = 0; y < sizeY.y_size; y++)
            memcpy(ost->frame->data[0] + y * ost->frame->linesize[0],
                   image_Y->data<uint16_t>() + y * sizeY.x_size, sizeY.x_size * sizeof(uint16_t));

        for (int y = 0; y < sizeU.y_size; y++) {
            memcpy(ost->frame->data[1] + y * ost->frame->linesize[1],
                   image_U->data<uint16_t>() + y * sizeU.x_size, sizeU.x_size * sizeof(uint16_t));
            memcpy(ost->frame->data[2] + y * ost->frame->linesize[2],
                   image_V->data<uint16_t>() + y * sizeU.x_size, sizeU.x_size * sizeof(uint16_t));
        }
    } else {
        for (int y = 0; y < sizeY.y_size; y++)
            for (int x = 0; x < sizeY.x_size; x++)
                ost->frame->data[0][y * ost->frame->linesize[0] + x] = image_Y->data<uint16_t>()[y * sizeY.x_size + x] >> 8;

        for (int y = 0; y < sizeU.y_size; y++)
            for (int x = 0; x < sizeU.x_size; x++) {
                ost->frame->data[1][y * ost->frame->linesize[1] + x] = image_U->data<uint16_t>()[y * sizeU.x_size + x] >> 8;
                ost->frame->data[2][y * ost->frame->linesize[2] + x] = image_V->data<uint16_t>()[y * sizeU.x_size + x] >> 8;
            }
    }

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}

bool VideoFileWriter::writeFrame(AVCodecContext *c, AVStream *st, AVFrame *frame, AVPacket *pkt) {
    // send the frame to the encoder; a null frame enters drain mode and
    // flushes the packets still buffered inside the encoder
    int ret = avcodec_send_frame(c, frame);
    if (ret < 0)
        throw std::runtime_error(std::format("Error sending a frame to the encoder: {}", av_err2string(ret)));

    while (true) {
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
            throw std::runtime_error(std::format("Error encoding a frame: {}", av_err2string(ret)));

        // rescale output packet timestamp values from codec to stream timebase
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        AVRational *time_base = &m_format_context->streams[pkt->stream_index]->time_base;
        m_log.debug(eOutput, std::format("pts:{} pts_time:{} dts:{} dts_time:{} duration:{} duration_time:{} stream_index:{}",
                                         av_ts2string(pkt->pts), av_ts2timestring(pkt->pts, time_base),
                                         av_ts2string(pkt->dts), av_ts2timestring(pkt->dts, time_base),
                                         av_ts2string(pkt->duration), av_ts2timestring(pkt->duration, time_base),
                                         pkt->stream_index));

        // Write the compressed frame to the media file
        ret = av_interleaved_write_frame(m_format_context, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0)
            throw std::runtime_error(std::format("Error while writing output packet: {}", av_err2string(ret)));
    }
    return ret == AVERROR_EOF;
}
