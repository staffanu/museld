// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_VIDEOFILEWRITER_H
#define MUSECPP_VIDEOFILEWRITER_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <format>
#include <vector>
#include "musevk/VulkanImage.h"
#include "logging/Logger.h"
#include "AudioDefs.h"
#include "VideoWriterOptions.h"
#include "VideoFileTimeline.h"

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
    // audio_streams: the file's audio streams, one per decoded track, each
    // PCM encoded to the preset's audio codec or, with ac3_passthrough, the
    // original AC3 bitstream (stream copy of the sync frames passed in).
    // With closed_captions the file has a text subtitle stream for the cues
    // passed to addClosedCaption (an NTSC disc's line-21 captions).
    VideoFileWriter(std::string filename, Logger &log, int video_width, int video_height,
                    int video_fps_num, int video_fps_den,
                    VideoWriterPreset preset, VideoColorStandard color_standard,
                    int display_aspect_num, int display_aspect_den,
                    std::vector<WriterAudioStream> audio_streams, bool closed_captions) :
            m_closed_captions(closed_captions),
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
            m_format_context(nullptr),
            m_have_video(false),
            m_encode_video(false),
            m_timeline(video_fps_num, video_fps_den) {
        for (auto &spec : audio_streams) {
            AudioStream stream;
            stream.spec = std::move(spec);
            m_audio.push_back(std::move(stream));
        }
    }
    VideoFileWriter(const VideoFileWriter &) = delete;
    VideoFileWriter(VideoFileWriter &&) = delete;
    VideoFileWriter &operator=(const VideoFileWriter &) = delete;
    VideoFileWriter &operator=(VideoFileWriter &&) = delete;

    bool init();
    // One audio stream's input of a frame, in the order of the constructor's
    // audio_streams; a stream given nothing (or fewer entries than streams)
    // gets no audio for the frame and catches up with silence
    struct AudioInput {
        AudioMode mode = MODE_UNKNOWN;
        int count = 0;
        const AudioFrame *samples = nullptr;
        const std::vector<std::array<uint8_t, 1536>> *ac3_frames = nullptr; // MODE_AC3: the sync frames
    };
    void addVideoFrameWithAudio(std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
                                std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V,
                                const std::vector<AudioInput> &audio,
                                std::optional<double> disc_seconds, std::optional<int> chapter);
    // The audio streams, as constructed
    [[nodiscard]] std::vector<WriterAudioStream> audioStreams() const {
        std::vector<WriterAudioStream> specs;
        for (const auto &stream : m_audio)
            specs.push_back(stream.spec);
        return specs;
    }
    // A closed caption cue for the subtitle stream.  start and end are the
    // cue's on-screen span in the input's playback time, now that time at
    // the frame last added: the cue is placed relative to that frame, so the
    // input's own clock need not match the file's (a cue is known complete
    // only once the next one replaces it, so they arrive after their frames)
    void addClosedCaption(double start_seconds, double end_seconds, double now_seconds,
                          const std::vector<std::string> &lines);
    // The frames that follow come from another input (the disc's next side)
    void newInput() { m_timeline.newInput(); }
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

    // One audio stream with its sync state against the video clock
    struct AudioStream {
        WriterAudioStream spec;
        OutputStream ost{};  // codec_context null for the passthrough stream
        bool encode = true;  // false after a write error: the stream is given up
        struct SwrContext *swr = nullptr; // resampling; created lazily when the input sample rate is known
        AudioMode input_mode = MODE_UNKNOWN; // audio mode the resampler was created for
        int in_channels = 2;  // AudioFrame slots the mode fills; the resampler downmixes to the file's stereo
        int64_t anchor_frame = 0; // video frame where the audio sync schedule starts
        int64_t in_samples = 0;   // input-rate samples fed to the resampler since the anchor
        int16_t last_sample[MAX_AUDIO_CHANNELS]{}; // last real sample written; start point for interpolation/fade-out
        int samples_in_frame = 0;
        int64_t ac3_next_pts = 0;      // passthrough: in samples at 48 kHz
        bool ac3_pause_logged = false; // passthrough: the track's audio left AC3 mid-write
    };

    void initStream(OutputStream *ost, const char *codec_name);
    void initVideo(const PresetSpec &spec);
    void initAudio(AudioStream &stream, const PresetSpec &spec);
    void initAudioPassthrough(AudioStream &stream);
    void initClosedCaptions(const PresetSpec &spec);
    void addAudio(AudioStream &stream, const AudioInput &input);
    void writeAc3Frames(AudioStream &stream, const std::vector<std::array<uint8_t, 1536>> &ac3_frames);
    void initResampler(AudioStream &stream, AudioMode mode);
    void convertAndWriteAudio(AudioStream &stream, const uint8_t **in, int in_samples);
    void insertSilence(AudioStream &stream, int64_t count, int in_rate, bool final_padding = false);
    void insertInterpolated(AudioStream &stream, int64_t count, int in_rate, const AudioFrame &next);
    AVFrame *makeVideoFrame(OutputStream *ost, std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
                            std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V);
    bool writeFrame(AVCodecContext *c, AVStream *st, AVFrame *frame, AVPacket *pkt);
    int mux(AVPacket *pkt, AVRational packet_time_base, AVStream *st);
    void writeHeader();
    void addChapters();
    static void close_stream(OutputStream *ost);

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

    const bool m_closed_captions;
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
    std::vector<AudioStream> m_audio;
    AVStream *m_caption_stream = nullptr; // the closed caption text stream, muxed without an encoder
    int64_t m_caption_last_pts = -1;      // ms; a cue is not written before the one preceding it
    AVFormatContext *m_format_context;
    bool m_have_video; // if the output supports video
    bool m_encode_video; // if we want to encode video

    uint8_t m_audio_tmp_buffer[MAX_AUDIO_OUTPUT_SAMPLES * MAX_AUDIO_CHANNELS * sizeof(int16_t)]; // shared by the streams, used one at a time

    // The disc time of the first frame goes into the container's header as
    // its start timecode, and is known only once the disc code runs steadily
    // (see VideoFileTimeline).  Until then the header is not written and the
    // packets wait here, in their encoder's time base: the muxer may change
    // a stream's time base when it writes the header.
    struct PendingPacket {
        AVPacket *packet;
        AVRational time_base;
        AVStream *stream;
    };
    VideoFileTimeline m_timeline;
    bool m_header_written = false;
    std::vector<PendingPacket> m_pending;
    int64_t m_pending_bytes = 0;
};

#endif //MUSECPP_VIDEOFILEWRITER_H
