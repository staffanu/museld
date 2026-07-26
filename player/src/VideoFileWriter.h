// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOFILEWRITER_H
#define MUSECPP_VIDEOFILEWRITER_H

#include <cstdint>
#include <string>
#include <format>
#include "musevk/VulkanImage.h"
#include "logging/Logger.h"
#include "AudioDefs.h"
#include "VideoWriterOptions.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

class VideoFileWriter {
public:
    VideoFileWriter(std::string filename, Logger &log, int video_width, int video_height,
                    int video_fps_num, int video_fps_den,
                    VideoWriterPreset preset, VideoColorStandard color_standard,
                    int display_aspect_num, int display_aspect_den) :
            m_filename(std::move(filename)),
            m_log(log),
            m_video_width(video_width),
            m_video_height(video_height),
            m_video_fps_num(video_fps_num),
            m_video_fps_den(video_fps_den),
            m_preset(preset),
            m_color_standard(color_standard),
            m_display_aspect{display_aspect_num, display_aspect_den},
            m_video_stream{},
            m_audio_stream{},
            m_format_context(nullptr),
            m_have_video(false),
            m_have_audio(false),
            m_encode_video(false),
            m_encode_audio(false),
            m_swr_ctx(nullptr),
            m_audio_input_mode(MODE_UNKNOWN),
            m_audio_anchor_frame(0),
            m_audio_in_samples(0),
            m_last_sample{},
            m_samples_in_frame(0) {
    }
    VideoFileWriter(const VideoFileWriter &) = delete;
    VideoFileWriter(VideoFileWriter &&) = delete;
    VideoFileWriter &operator=(const VideoFileWriter &) = delete;
    VideoFileWriter &operator=(VideoFileWriter &&) = delete;

    bool init();
    void addVideoFrameWithAudio(std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
                                std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V,
                                AudioMode audio_mode, int number_of_samples, AudioFrame *audio_samples);
    void cleanup();

private:
    // wrapper around a single output AVStream -- video or audio
    struct OutputStream {
        AVStream *stream;
        const AVCodec *codec;
        AVCodecContext *codec_context;

        int64_t next_pts; // pts of the next frame that will be generated

        AVFrame *frame;
        AVPacket *tmp_pkt;
    };

    // What each preset maps to: container, encoders, and pixel format.
    struct PresetSpec {
        const char *format_name;
        const char *video_codec;
        const char *audio_codec;
        AVPixelFormat pix_fmt;
    };
    static PresetSpec presetSpec(VideoWriterPreset preset);

    void initStream(OutputStream *ost, const char *codec_name);
    void initVideo(const PresetSpec &spec);
    void initAudio(const PresetSpec &spec);
    void initResampler(AudioMode mode);
    void convertAndWriteAudio(const uint8_t **in, int in_samples);
    void insertSilence(int64_t count, int in_rate);
    void insertInterpolated(int64_t count, int in_rate, const AudioFrame &next);
    AVFrame *makeVideoFrame(OutputStream *ost, std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
                            std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V);
    bool writeFrame(AVCodecContext *c, AVStream *st, AVFrame *frame, AVPacket *pkt);
    static void close_stream(AVFormatContext *oc, OutputStream *ost);

    static std::string av_err2string(int errnum) {
        char str[AV_ERROR_MAX_STRING_SIZE];
        return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
    }

    static std::string av_ts2string(int64_t ts) {
        char str[AV_TS_MAX_STRING_SIZE];
        return av_ts_make_string(str, ts);
    }

    static std::string av_ts2timestring(int64_t ts, AVRational *tb) {
        char str[AV_TS_MAX_STRING_SIZE];
        return av_ts_make_time_string(str, ts, tb);
    }

    std::string m_filename;
    Logger &m_log;
    int m_video_width;
    int m_video_height;
    int m_video_fps_num; // frames per second as a rational, e.g. 60000/1001
    int m_video_fps_den;
    VideoWriterPreset m_preset;
    VideoColorStandard m_color_standard;
    AVRational m_display_aspect;
    OutputStream m_video_stream;
    OutputStream m_audio_stream;
    AVFormatContext *m_format_context;
    bool m_have_video; // if the output supports video
    bool m_have_audio; // if the output supports audio
    bool m_encode_video; // if we want to encode video
    bool m_encode_audio; // if we want to encode audio

    struct SwrContext *m_swr_ctx; // resampling; created lazily when the input sample rate is known
    AudioMode m_audio_input_mode; // audio mode the resampler was created for

    int64_t m_audio_anchor_frame; // video frame where the audio sync schedule starts
    int64_t m_audio_in_samples;   // input-rate samples fed to the resampler since the anchor
    int16_t m_last_sample[2];     // last real sample written; start point for interpolation/fade-out

    uint8_t m_audio_tmp_buffer[20000];
    int m_samples_in_frame;
};

#endif //MUSECPP_VIDEOFILEWRITER_H
