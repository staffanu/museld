//
// Created by staffanu on 3/7/24.
//

#ifndef MUSECPP_VIDEOFILEWRITER_H
#define MUSECPP_VIDEOFILEWRITER_H

#include <string>
#include <format>
#include "musevk/VulkanImage.h"
#include "util/Logger.h"
#include "AudioDefs.h"

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
    VideoFileWriter(std::string filename, Logger &log, int video_width, int video_height, int video_frame_rate) :
            m_filename(std::move(filename)),
            m_log(log),
            m_video_width(video_width),
            m_video_height(video_height),
            m_video_frame_rate(video_frame_rate),
            m_video_stream{},
            m_audio_stream{},
            m_format_context(nullptr),
            m_have_video(false),
            m_have_audio(false),
            m_encode_video(false),
            m_encode_audio(false),
            m_swr_ctx(nullptr),
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

    static constexpr int64_t c_video_output_bit_rate = 5000000;

    void initStream(OutputStream *ost, enum AVCodecID codec_id);
    void initVideo(AVDictionary *opt_arg);
    void initAudio(AVDictionary *opt_arg);
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
    int m_video_frame_rate;
    OutputStream m_video_stream;
    OutputStream m_audio_stream;
    AVFormatContext *m_format_context;
    bool m_have_video; // if the output supports video
    bool m_have_audio; // if the output supports audio
    bool m_encode_video; // if we want to encode video
    bool m_encode_audio; // if we want to encode audio

    struct SwrContext *m_swr_ctx; // resampling

    uint8_t m_audio_tmp_buffer[20000];
    int m_samples_in_frame;
};

#endif //MUSECPP_VIDEOFILEWRITER_H
