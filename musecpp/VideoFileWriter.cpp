//
// Created by staffanu on 3/7/24.
//

#include "VideoFileWriter.h"

bool VideoFileWriter::init() {
    avformat_alloc_output_context2(&m_format_context, nullptr, nullptr, m_filename.c_str());
    if (!m_format_context) {
        m_log.info(eVideo, "Could not deduce output format from filename extension");
        return false;
    }

    AVDictionary *opt = nullptr;
//        av_dict_set(&opt, "flags", "???", 0);
//        av_dict_set(&opt, "fflags", "???", 0);

    if (m_format_context->oformat->video_codec != AV_CODEC_ID_NONE) {
        initVideo(opt);
        m_have_video = true;
        m_encode_video = true;
    }

    if (m_format_context->oformat->audio_codec != AV_CODEC_ID_NONE) {
        initAudio(opt);
        m_have_audio = true;
        m_encode_audio = true;
    }

    av_dump_format(m_format_context, 0, m_filename.c_str(), 1);

    // open the output file, if needed
    if (!(m_format_context->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&m_format_context->pb, m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
            throw std::runtime_error(fmt::format("Could not open '{}': {}}", m_filename, av_err2string(ret)));
    }

    // Write the stream header, if any
    int ret = avformat_write_header(m_format_context, &opt);
    if (ret < 0)
        throw std::runtime_error(fmt::format("Error occurred when opening output file: {}", av_err2string(ret)));

    return true;
}

void VideoFileWriter::addVideoFrameWithAudio(
        std::shared_ptr<musevk::VulkanImage> const &image,
        AudioMode audio_mode, int number_of_samples, AudioDecoder::AudioFrame *audio_samples) {

    //av_compare_ts(m_video_stream.next_pts, m_video_stream.codec_context->time_base,
    //              m_audio_stream.next_pts, m_audio_stream.codec_context->time_base)

    if (m_encode_video) {
        assert(image->getWidth() == m_video_stream.codec_context->width);
        assert(image->getHeight() == m_video_stream.codec_context->height);
        auto frame = makeVideoFrame(&m_video_stream, image);
        m_encode_video = !writeFrame(m_video_stream.codec_context, m_video_stream.stream, frame, m_video_stream.tmp_pkt);
    }

    if (number_of_samples != 0 && m_encode_audio) {

        int ret = av_frame_make_writable(m_audio_stream.frame);
        if (ret < 0)
            throw std::runtime_error("Failed to make audio frame writable");

        auto *q = (int16_t *) m_audio_tmp_buffer;
        for (int i = 0; i < number_of_samples; i++)
            for (int j = 0; j < m_audio_stream.codec_context->ch_layout.nb_channels; j++)
                *q++ = audio_samples[i].samples[j];

        int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)m_audio_stream.frame->format);

        bool filled_frame;
        do {
            uint8_t *out[8];
            if (av_sample_fmt_is_planar((AVSampleFormat)m_audio_stream.frame->format)) {
                for (int i = 0; i < m_audio_stream.codec_context->ch_layout.nb_channels; i++)
                    out[i] = {m_audio_stream.frame->data[i] + m_samples_in_frame * bytes_per_sample};
            } else {
                assert(false); // easy to add support if needed
            }
            const uint8_t *in[8] = {m_audio_tmp_buffer};

            ret = swr_convert(m_swr_ctx, out,
                              m_audio_stream.frame->nb_samples - m_samples_in_frame,
                              in, number_of_samples);
            number_of_samples = 0;
            if (ret < 0)
                throw std::runtime_error("Error while converting audio");

            m_samples_in_frame += ret;

            assert (m_samples_in_frame <= m_audio_stream.frame->nb_samples);
            filled_frame =  m_samples_in_frame == m_audio_stream.frame->nb_samples;
            if (filled_frame) {

                m_encode_audio = !writeFrame(m_audio_stream.codec_context, m_audio_stream.stream, m_audio_stream.frame,
                                             m_audio_stream.tmp_pkt);

                assert(av_frame_is_writable(m_audio_stream.frame)); // it seems like audio frames are always writable after making writable the first time?

                m_audio_stream.frame->pts = m_audio_stream.next_pts;
                m_audio_stream.next_pts += m_audio_stream.frame->nb_samples;
                m_samples_in_frame = 0;
            }
        } while (filled_frame);
    }
}

void VideoFileWriter::cleanup() {
    av_write_trailer(m_format_context);

    if (m_have_video)
        close_stream(m_format_context, &m_video_stream);
    if (m_have_audio)
        close_stream(m_format_context, &m_audio_stream);

    sws_freeContext(m_sws_ctx);
    swr_free(&m_swr_ctx);

    if (!(m_format_context->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m_format_context->pb); // Close the output file

    avformat_free_context(m_format_context);
}

void VideoFileWriter::initStream(OutputStream *ost, enum AVCodecID codec_id) {
    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt)
        throw std::runtime_error("Could not allocate AVPacket");

    ost->stream = avformat_new_stream(m_format_context, nullptr);
    if (!ost->stream)
        throw std::runtime_error("Could not allocate stream");
    ost->stream->id = m_format_context->nb_streams - 1;

    ost->codec = avcodec_find_encoder(codec_id);
    if (!ost->codec)
        throw std::runtime_error(fmt::format("Could not find encoder for {}", avcodec_get_name(codec_id)));

    AVCodecContext *c = avcodec_alloc_context3(ost->codec);
    if (!c)
        throw std::runtime_error("Could not alloc an encoding context");
    ost->codec_context = c;

    // Some formats want stream headers to be separate
    if (m_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

void VideoFileWriter::initVideo(AVDictionary *opt_arg) {
    initStream(&m_video_stream, m_format_context->oformat->video_codec);

    m_video_stream.stream->sample_aspect_ratio = { 16 * 1032, 9 * 1122 };

    AVCodecContext *c = m_video_stream.codec_context;
    c->bit_rate = c_video_output_bit_rate;
    c->width = m_video_width;   // Resolution must be a multiple of two.
    c->height = m_video_height;
    m_video_stream.stream->time_base = (AVRational) {1, m_video_frame_rate};
    c->time_base = m_video_stream.stream->time_base;
    c->gop_size = 12; // emit one intra frame every twelve frames at most
    c->pix_fmt = AV_PIX_FMT_YUV420P;

    AVDictionary *opt = nullptr;
    av_dict_copy(&opt, opt_arg, 0);
    int ret = avcodec_open2(c, m_video_stream.codec, &opt);
    av_dict_free(&opt);
    if (ret < 0)
        throw std::runtime_error(fmt::format("Could not open video codec: {}", av_err2string(ret)));

    m_video_stream.frame = allocFrame(c->pix_fmt, c->width, c->height);

    // If the output format is not AV_PIX_FMT_BGRA, then a temporary AV_PIX_FMT_BGRA
    //picture is needed too.  It is then converted to the required output format.
    m_video_stream.tmp_frame = nullptr;
    if (c->pix_fmt != AV_PIX_FMT_BGRA) {
        m_video_stream.tmp_frame = allocFrame(AV_PIX_FMT_BGRA, c->width, c->height);

        m_sws_ctx = sws_getContext(c->width, c->height,
                                   AV_PIX_FMT_BGRA,
                                   c->width, c->height,
                                   c->pix_fmt,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!m_sws_ctx)
            throw std::runtime_error("Could not initialize the conversion context");
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(m_video_stream.stream->codecpar, c);
    if (ret < 0)
        throw std::runtime_error("Could not copy the stream parameters");
}

AVFrame *VideoFileWriter::allocFrame(enum AVPixelFormat pix_fmt, int width, int height) {
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        throw std::runtime_error("Could not allocate video frame");

    frame->format = pix_fmt;
    frame->width = width;
    frame->height = height;

    /* allocate the buffers for the frame data */
    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        throw std::runtime_error("Could not allocate video frame buffer");

    return frame;
}

void VideoFileWriter::initAudio(AVDictionary *opt_arg) {
    initStream(&m_audio_stream, m_format_context->oformat->audio_codec);

    AVCodecContext *c = m_audio_stream.codec_context;
    c->sample_fmt = m_audio_stream.codec->sample_fmts ? m_audio_stream.codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate = 64000;
    c->sample_rate = 44100;
    if (m_audio_stream.codec->supported_samplerates) {
        c->sample_rate = m_audio_stream.codec->supported_samplerates[0];
        for (int i = 0; m_audio_stream.codec->supported_samplerates[i]; i++) {
            if (m_audio_stream.codec->supported_samplerates[i] == 44100)
                c->sample_rate = 44100;
        }
    }
    auto layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&c->ch_layout, &layout);
    m_audio_stream.stream->time_base = (AVRational) {1, c->sample_rate};

    AVDictionary *opt = nullptr;
    av_dict_copy(&opt, opt_arg, 0);
    int ret = avcodec_open2(c, m_audio_stream.codec, &opt);
    av_dict_free(&opt);
    if (ret < 0)
        throw std::runtime_error(fmt::format("Could not open audio codec: {}", av_err2string(ret)));

    int nb_samples = c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE ? 1024 : c->frame_size;

    m_audio_stream.frame = allocAudioFrame(c->sample_fmt, &c->ch_layout, c->sample_rate, nb_samples);
    m_audio_stream.tmp_frame = allocAudioFrame(AV_SAMPLE_FMT_S16, &c->ch_layout, c->sample_rate, nb_samples);

    // copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(m_audio_stream.stream->codecpar, c);
    if (ret < 0)
        throw std::runtime_error("Could not copy the stream parameters");

    // create and initialize resampler context
    m_swr_ctx = swr_alloc();
    if (!m_swr_ctx)
        throw std::runtime_error("Could not allocate resampler context");

    assert(av_opt_set_chlayout(m_swr_ctx, "in_chlayout", &c->ch_layout, 0) == 0);
    assert(av_opt_set_int(m_swr_ctx, "in_sample_rate", 32000, 0) == 0);
    assert(av_opt_set_sample_fmt(m_swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0) == 0);
    assert(av_opt_set_chlayout(m_swr_ctx, "out_chlayout", &c->ch_layout, 0) == 0);
    assert(av_opt_set_int(m_swr_ctx, "out_sample_rate", c->sample_rate, 0) == 0);
    assert(av_opt_set_sample_fmt(m_swr_ctx, "out_sample_fmt", c->sample_fmt, 0) == 0);

    ret = swr_init(m_swr_ctx);
    if (ret < 0)
        throw std::runtime_error("Failed to initialize the resampling context");
}

AVFrame *VideoFileWriter::allocAudioFrame(enum AVSampleFormat sample_fmt,
                                          const AVChannelLayout *channel_layout,
                                          int sample_rate, int nb_samples) {
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        throw std::runtime_error("Error allocating an audio frame");

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;
    frame->pts = 0;

    if (nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0)
            throw std::runtime_error("Error allocating an audio buffer");
    }

    return frame;
}

void VideoFileWriter::close_stream(AVFormatContext *oc, OutputStream *ost) {
    avcodec_free_context(&ost->codec_context);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
}

AVFrame *VideoFileWriter::makeVideoFrame(OutputStream *ost, std::shared_ptr<musevk::VulkanImage> const &image) {
    AVCodecContext *c = ost->codec_context;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
        throw std::runtime_error("Failed to make video frame writable");

    if (c->pix_fmt != AV_PIX_FMT_BGRA) {
        copyImageToFrame(ost->tmp_frame, image);

        sws_scale(m_sws_ctx, (const uint8_t *const *) ost->tmp_frame->data,
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        copyImageToFrame(ost->frame, image);
    }

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}

void VideoFileWriter::copyImageToFrame(AVFrame *pict, std::shared_ptr<musevk::VulkanImage> const &image) {
    for (int y = 0; y < image->getHeight(); y++)
        // FIXME: I haven't figured out how to feed linear RGB values to swscale, so apply a 2.2 inverse gamma
        for (int x = 0; x < image->getWidth() * 4; x++)
            pict->data[0][y * pict->linesize[0] + x] = pow(image->data<uint8_t>()[y * image->getWidth() * 4 + x] / 256.0, 1/2.2) * 256;
        //memcpy(pict->data[0] + y * pict->linesize[0], image->data<uint32_t>() + y * image->getWidth(), image->getWidth() * 4);
}

bool VideoFileWriter::writeFrame(AVCodecContext *c, AVStream *st, AVFrame *frame, AVPacket *pkt) {
    // send the frame to the encoder
    int ret = avcodec_send_frame(c, frame);
    if (ret < 0)
        throw std::runtime_error(fmt::format("Error sending a frame to the encoder: {}", av_err2string(ret)));

    while (ret >= 0) {
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
            throw std::runtime_error(fmt::format("Error encoding a frame: {}", av_err2string(ret)));

        // rescale output packet timestamp values from codec to stream timebase
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        AVRational *time_base = &m_format_context->streams[pkt->stream_index]->time_base;
        m_log.debug(eOutput, fmt::format("pts:{} pts_time:{} dts:{} dts_time:{} duration:{} duration_time:{} stream_index:{}",
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
            throw std::runtime_error(fmt::format("Error while writing output packet: {}", av_err2string(ret)));
    }
    return ret == AVERROR_EOF;
}
