// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <format>
#include "CompressedAudioDecoder.h"
#include "logging/Logger.h"

#ifdef HAVE_LIBAV

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

struct CompressedAudioDecoder::Impl {
    const AVCodec *codec = nullptr;
    AVCodecContext *ctx = nullptr;
    AVCodecParserContext *parser = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    // Converts each decoded frame to interleaved stereo s16, downmixing when
    // the decoder ignored the downmix request (the dca decoder does);
    // reconfigured whenever the frame's layout/format/rate changes
    SwrContext *swr = nullptr;
    AVChannelLayout swr_in_layout = {};
    AVSampleFormat swr_in_format = AV_SAMPLE_FMT_NONE;
    int swr_in_rate = 0;
    int swr_out_channels = 0;
    std::vector<int16_t> stereo_buffer; // staging when the output is narrower than an AudioFrame

    // Silence inserted per undecodable frame; refined to the stream's real
    // frame length after the first successful decode
    int error_silence_samples;

    ~Impl() {
        swr_free(&swr);
        av_channel_layout_uninit(&swr_in_layout);
        av_frame_free(&frame);
        av_packet_free(&packet);
        av_parser_close(parser);
        avcodec_free_context(&ctx);
    }
};

CompressedAudioDecoder::CompressedAudioDecoder(Logger &log, Codec codec)
: m_log(log), m_codec(codec), m_unavailable_warned(false) {
    const AVCodecID codec_id = codec == Codec::eAc3 ? AV_CODEC_ID_AC3 : AV_CODEC_ID_DTS;
    auto impl = std::make_unique<Impl>();
    impl->error_silence_samples = codec == Codec::eAc3 ? 1536 : 512;
    impl->codec = avcodec_find_decoder(codec_id);
    impl->parser = impl->codec ? av_parser_init(codec_id) : nullptr;
    impl->ctx = impl->codec ? avcodec_alloc_context3(impl->codec) : nullptr;
    if (impl->codec == nullptr || impl->parser == nullptr || impl->ctx == nullptr) {
        m_log.error(eAudio, std::format("libavcodec has no {} decoder; this audio track will be silent",
                                        codec == Codec::eAc3 ? "AC3" : "DTS"));
        return;
    }
    if (avcodec_open2(impl->ctx, impl->codec, nullptr) < 0) {
        m_log.error(eAudio, "libavcodec decoder failed to open; this audio track will be silent");
        return;
    }
    impl->packet = av_packet_alloc();
    impl->frame = av_frame_alloc();
    m_impl = std::move(impl);
}

CompressedAudioDecoder::~CompressedAudioDecoder() = default;

bool CompressedAudioDecoder::available() { return true; }

std::vector<AudioFrame> CompressedAudioDecoder::decode(const uint8_t *data, size_t size) {
    std::vector<AudioFrame> output;
    if (m_impl == nullptr)
        return output;
    Impl &impl = *m_impl;

    auto convertFrame = [&]() -> bool {
        if (impl.swr == nullptr || av_channel_layout_compare(&impl.swr_in_layout, &impl.frame->ch_layout) != 0
            || impl.swr_in_format != (AVSampleFormat)impl.frame->format
            || impl.swr_in_rate != impl.frame->sample_rate) {
            swr_free(&impl.swr);
            // Mono/stereo passes through; anything wider is remapped to the
            // AudioFrame 5.1 order (extra channels of exotic layouts fold in,
            // and 5.1(back) lands on the side slots)
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            const AVChannelLayout surround = AV_CHANNEL_LAYOUT_5POINT1;
            const bool to_surround = impl.frame->ch_layout.nb_channels > 2;
            const AVChannelLayout &out_layout = to_surround ? surround : stereo;
            if (swr_alloc_set_opts2(&impl.swr, &out_layout, AV_SAMPLE_FMT_S16, impl.frame->sample_rate,
                                    &impl.frame->ch_layout, (AVSampleFormat)impl.frame->format,
                                    impl.frame->sample_rate, 0, nullptr) < 0)
                return false;
            // Normalize any mixing matrix so a full-scale input cannot clip
            av_opt_set_double(impl.swr, "rematrix_maxval", 1.0, 0);
            if (swr_init(impl.swr) < 0) {
                swr_free(&impl.swr);
                return false;
            }
            av_channel_layout_copy(&impl.swr_in_layout, &impl.frame->ch_layout);
            impl.swr_in_format = (AVSampleFormat)impl.frame->format;
            impl.swr_in_rate = impl.frame->sample_rate;
            impl.swr_out_channels = out_layout.nb_channels;
        }
        // Equal in and out rates: the conversion is 1:1 and buffers nothing
        const size_t base = output.size();
        int converted;
        if (impl.swr_out_channels == MAX_AUDIO_CHANNELS) {
            // An AudioFrame is exactly the interleaved 5.1 sample, so convert in place
            output.resize(base + impl.frame->nb_samples);
            uint8_t *out[1] = {(uint8_t *)&output[base]};
            converted = swr_convert(impl.swr, out, impl.frame->nb_samples,
                                    (const uint8_t **)impl.frame->extended_data, impl.frame->nb_samples);
            output.resize(base + std::max(converted, 0));
        } else {
            impl.stereo_buffer.resize((size_t)impl.frame->nb_samples * impl.swr_out_channels);
            uint8_t *out[1] = {(uint8_t *)impl.stereo_buffer.data()};
            converted = swr_convert(impl.swr, out, impl.frame->nb_samples,
                                    (const uint8_t **)impl.frame->extended_data, impl.frame->nb_samples);
            for (int i = 0; i < converted; i++) {
                AudioFrame f{};
                for (int ch = 0; ch < impl.swr_out_channels; ch++)
                    f.samples[ch] = impl.stereo_buffer[i * impl.swr_out_channels + ch];
                output.push_back(f);
            }
        }
        return converted > 0;
    };

    auto receiveFrames = [&]() {
        while (avcodec_receive_frame(impl.ctx, impl.frame) == 0) {
            impl.error_silence_samples = impl.frame->nb_samples;
            convertFrame();
        }
    };

    while (size > 0) {
        uint8_t *frame_data = nullptr;
        int frame_size = 0;
        int consumed = av_parser_parse2(impl.parser, impl.ctx, &frame_data, &frame_size,
                                        data, (int)size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0)
            break; // the parser is stuck; drop the rest and resync on later input
        data += consumed;
        size -= consumed;
        if (frame_size == 0)
            continue;
        impl.packet->data = frame_data;
        impl.packet->size = frame_size;
        const size_t before = output.size();
        if (avcodec_send_packet(impl.ctx, impl.packet) == 0)
            receiveFrames();
        if (output.size() == before) // the frame was damaged: hold the timeline with silence
            output.resize(before + impl.error_silence_samples, AudioFrame{});
    }
    return output;
}

#else // !HAVE_LIBAV

struct CompressedAudioDecoder::Impl {};

CompressedAudioDecoder::CompressedAudioDecoder(Logger &log, Codec codec)
: m_log(log), m_codec(codec), m_unavailable_warned(false) {
}

CompressedAudioDecoder::~CompressedAudioDecoder() = default;

bool CompressedAudioDecoder::available() { return false; }

std::vector<AudioFrame> CompressedAudioDecoder::decode(const uint8_t *, size_t) {
    if (!m_unavailable_warned) {
        m_unavailable_warned = true;
        m_log.warn(eAudio, std::format("This build has no FFmpeg, so the {} audio cannot be decoded "
                                       "and stays silent",
                                       m_codec == Codec::eAc3 ? "AC3" : "DTS"));
    }
    return {};
}

#endif
