// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
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

// The most the packets waiting for the header may hold.  The standard preset
// never comes near it in the minute the timeline waits for a steady disc
// code; the lossless preset reaches it in a few seconds of HD video, and a
// file whose disc code starts later than that goes without a start timecode.
static constexpr int64_t MAX_PENDING_BYTES = 256LL << 20;

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
        case MODE_ANALOG: return 48000;
        case MODE_AC3: return 48000;
        case MODE_DTS: return 44100;
        default:       return 0;
    }
}

// AudioFrame slots the mode fills.  The 5.1 modes are downmixed to the file's
// stereo track by the resampler; MODE_A's back channels are still dropped.
static int audioModeChannels(AudioMode mode) {
    return mode == MODE_AC3 || mode == MODE_DTS ? MAX_AUDIO_CHANNELS : 2;
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

    for (auto &stream : m_audio) {
        if (stream.spec.ac3_passthrough)
            initAudioPassthrough(stream);
        else
            initAudio(stream, spec);
        if (!stream.spec.title.empty())
            av_dict_set(&stream.ost.stream->metadata, "title", stream.spec.title.c_str(), 0);
    }

    if (m_closed_captions)
        initClosedCaptions(spec);

    av_dump_format(m_format_context, 0, m_filename.c_str(), 1);

    // open the output file, if needed
    if (!(m_format_context->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&m_format_context->pb, m_filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
            throw std::runtime_error(std::format("Could not open '{}': {}", m_filename, av_err2string(ret)));
    }

    // The header is written by writeHeader(), once the timeline knows the
    // start timecode that goes into it
    return true;
}

// Writes the container header, with the start timecode if the disc gave one,
// and then the packets that waited for it
void VideoFileWriter::writeHeader() {
    if (m_header_written)
        return;
    m_timeline.giveUp();
    if (auto timecode = m_timeline.startTimecode()) {
        av_dict_set(&m_format_context->metadata, "timecode", timecode->c_str(), 0);
        m_log.info(eOutput, std::format("{}: start timecode {} at {} frames/s (the first frame is at {:+.3f} s of disc time)",
                                        m_filename, *timecode, m_timeline.timecodeFps(),
                                        (double)*m_timeline.startFrames() * m_video_fps_den / m_video_fps_num));
    } else {
        m_log.info(eOutput, std::format("{}: no start timecode, the disc code gave no steady time", m_filename));
    }

    int ret = avformat_write_header(m_format_context, nullptr);
    if (ret < 0)
        throw std::runtime_error(std::format("Error occurred when opening output file: {}", av_err2string(ret)));
    m_header_written = true;

    std::vector<PendingPacket> pending;
    pending.swap(m_pending);
    m_pending_bytes = 0;
    int write_error = 0;
    for (auto &p : pending) {
        if (write_error == 0)
            write_error = mux(p.packet, p.time_base, p.stream);
        av_packet_free(&p.packet);
    }
    if (write_error < 0)
        throw std::runtime_error(std::format("Error while writing output packet: {}", av_err2string(write_error)));
}

// Hands a packet to the muxer, or keeps it until the header is written.
// Takes the packet's contents either way; returns the muxer's error, if any.
int VideoFileWriter::mux(AVPacket *pkt, AVRational packet_time_base, AVStream *st) {
    if (!m_header_written) {
        AVPacket *kept = av_packet_alloc();
        if (!kept)
            throw std::runtime_error("Could not allocate AVPacket");
        av_packet_move_ref(kept, pkt);
        m_pending_bytes += kept->size;
        m_pending.push_back({kept, packet_time_base, st});
        return 0;
    }

    // rescale the packet's timestamps from its encoder's to the stream's time base
    av_packet_rescale_ts(pkt, packet_time_base, st->time_base);
    pkt->stream_index = st->index;

    if (m_log.isEnabled(eDebug, eOutput))
        m_log.debug(eOutput, std::format("pts:{} pts_time:{} dts:{} dts_time:{} duration:{} duration_time:{} stream_index:{}",
                                         av_ts2string(pkt->pts), av_ts2timestring(pkt->pts, &st->time_base),
                                         av_ts2string(pkt->dts), av_ts2timestring(pkt->dts, &st->time_base),
                                         av_ts2string(pkt->duration), av_ts2timestring(pkt->duration, &st->time_base),
                                         pkt->stream_index));

    // av_interleaved_write_frame() takes ownership of the packet's contents
    // and resets it, so no unreferencing is necessary
    return av_interleaved_write_frame(m_format_context, pkt);
}

// The chapters the timeline found, each running to the next and the last to
// the end of the video.  Both muxers write chapters that appear before the
// trailer.  The MP4 muxer moves a first chapter that starts later to the
// start of the file and all that follow with it, so the stretch before the
// first mark (lead-in, pre-roll, frames without a code) is a chapter too.
void VideoFileWriter::addChapters() {
    struct Mark {
        int64_t frame;
        std::string title;
    };
    std::vector<Mark> marks;
    for (const auto &chapter : m_timeline.chapters()) {
        if (marks.empty() && chapter.frame > 0)
            marks.push_back({0, "Start"});
        marks.push_back({chapter.frame, std::format("Chapter {}", chapter.number)});
    }
    if (marks.empty() || m_format_context->nb_chapters != 0)
        return;
    auto **list = (AVChapter **) av_calloc(marks.size(), sizeof(AVChapter *));
    if (!list)
        return;
    unsigned int count = 0;
    for (size_t i = 0; i < marks.size(); i++) {
        auto *chapter = (AVChapter *) av_mallocz(sizeof(AVChapter));
        if (!chapter)
            break;
        chapter->id = (int64_t)i + 1;
        chapter->time_base = (AVRational){m_video_fps_den, m_video_fps_num};
        chapter->start = marks[i].frame;
        chapter->end = i + 1 < marks.size() ? marks[i + 1].frame : m_video_stream.next_pts;
        av_dict_set(&chapter->metadata, "title", marks[i].title.c_str(), 0);
        list[count++] = chapter;
    }
    m_format_context->chapters = list;
    m_format_context->nb_chapters = count;
    m_log.info(eOutput, std::format("{}: {} chapter mark{}", m_filename, m_timeline.chapters().size(),
                                    m_timeline.chapters().size() == 1 ? "" : "s"));
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
    m_video_stream.stream->time_base = (AVRational) {m_video_fps_den, m_video_fps_num};
    // The frame rate, said outright: the MP4 muxer counts the start timecode in it
    m_video_stream.stream->avg_frame_rate = (AVRational) {m_video_fps_num, m_video_fps_den};
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
        case VideoColorStandard::eBt470bg:
            // EBU primaries and matrix; the transfer is tagged BT.709 (the
            // same curve as SMPTE 170M) rather than BT.470's nominal gamma
            // 2.8, which no PAL material was ever actually encoded with
            c->colorspace = AVCOL_SPC_BT470BG;
            c->color_primaries = AVCOL_PRI_BT470BG;
            c->color_trc = AVCOL_TRC_BT709;
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


// The closed caption stream: text cues, muxed as the container's own text
// subtitle format (SubRip blocks in Matroska, 3GPP timed text in MP4) with
// no encoder in between.  The cues reach the muxer late, each when the next
// replaces it on screen, and the muxer's interleaver would otherwise hold
// the video and audio for its default ten seconds waiting for this stream:
// one second keeps the memory it takes bounded, and a cue later than that
// is written where it arrives, a block with an earlier time than the ones
// around it, which both containers allow.
void VideoFileWriter::initClosedCaptions(const PresetSpec &spec) {
    const bool matroska = std::string(spec.format_name) == "matroska";
    const AVCodecID codec_id = matroska ? AV_CODEC_ID_SUBRIP : AV_CODEC_ID_MOV_TEXT;
    if (avformat_query_codec(m_format_context->oformat, codec_id, FF_COMPLIANCE_NORMAL) != 1)
        throw std::runtime_error(std::format("Text subtitles cannot be stored in format {}", m_format_context->oformat->name));
    m_caption_stream = avformat_new_stream(m_format_context, nullptr);
    if (!m_caption_stream)
        throw std::runtime_error("Could not allocate stream");
    m_caption_stream->id = (int)m_format_context->nb_streams - 1;
    AVCodecParameters *par = m_caption_stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_SUBTITLE;
    par->codec_id = codec_id;
    m_caption_stream->time_base = (AVRational){1, 1000};
    av_dict_set(&m_caption_stream->metadata, "title", "Closed captions", 0);
    m_format_context->max_interleave_delta = AV_TIME_BASE;
}

void VideoFileWriter::addClosedCaption(double start_seconds, double end_seconds, double now_seconds,
                                       const std::vector<std::string> &lines) {
    if (!m_caption_stream || lines.empty() || end_seconds <= start_seconds)
        return;
    const double fps = (double)m_video_fps_num / m_video_fps_den;
    const double file_now = (double)m_video_stream.next_pts / fps;
    const int64_t pts = std::llround((file_now - (now_seconds - start_seconds)) * 1000);
    const int64_t duration = std::llround((end_seconds - start_seconds) * 1000);
    if (pts < 0 || pts < m_caption_last_pts) {
        m_log.debug(eOutput, std::format("Closed caption at {} ms dropped: before the stream's last cue at {} ms",
                                         pts, m_caption_last_pts));
        return;
    }
    std::string text;
    for (const auto &line : lines) {
        if (!text.empty()) text += '\n';
        text += line;
    }
    const bool mov_text = m_caption_stream->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT;
    if (mov_text && text.size() > 0xffff)
        text.resize(0xffff);
    AVPacket *pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, (int)text.size() + (mov_text ? 2 : 0)) < 0)
        throw std::runtime_error("Could not allocate subtitle packet");
    uint8_t *q = pkt->data;
    if (mov_text) { // 3GPP timed text: a 16-bit big-endian length before the UTF-8 text
        *q++ = (uint8_t)(text.size() >> 8);
        *q++ = (uint8_t)(text.size() & 0xff);
    }
    memcpy(q, text.data(), text.size());
    pkt->pts = pkt->dts = pts;
    pkt->duration = duration;
    pkt->flags |= AV_PKT_FLAG_KEY;
    m_caption_last_pts = pts;
    int ret = mux(pkt, (AVRational){1, 1000}, m_caption_stream);
    av_packet_free(&pkt);
    if (ret < 0)
        m_log.error(eOutput, std::format("Error while writing subtitle packet: {}", av_err2string(ret)));
}




void VideoFileWriter::addVideoFrameWithAudio(
        std::shared_ptr<musevk::VulkanBuffer> const &image_Y,
        std::shared_ptr<musevk::VulkanBuffer> const &image_U, std::shared_ptr<musevk::VulkanBuffer> const &image_V,
        const std::vector<AudioInput> &audio,
        std::optional<double> disc_seconds, std::optional<int> chapter) {

    m_timeline.addFrame(disc_seconds, chapter);
    if (!m_header_written && (m_timeline.startDecided() || m_pending_bytes > MAX_PENDING_BYTES))
        writeHeader();

    if (m_encode_video) {
        assert(image_Y->size().x_size == m_video_stream.codec_context->width);
        assert(image_Y->size().y_size == m_video_stream.codec_context->height);
        auto frame = makeVideoFrame(&m_video_stream, image_Y, image_U, image_V);
        m_encode_video = !writeFrame(m_video_stream.codec_context, m_video_stream.stream, frame, m_video_stream.tmp_pkt);
    }

    for (size_t i = 0; i < m_audio.size(); i++)
        addAudio(m_audio[i], i < audio.size() ? audio[i] : AudioInput{});
}

// One stream's audio of the frame just written
void VideoFileWriter::addAudio(AudioStream &stream, const AudioInput &input) {
    if (stream.spec.ac3_passthrough) {
        if (stream.encode && input.ac3_frames)
            writeAc3Frames(stream, *input.ac3_frames);
        // The stream is the AC3 bitstream: PCM from another track selected
        // mid-write has nowhere to go
        if ((!input.ac3_frames || input.ac3_frames->empty()) && input.count != 0 && input.mode != MODE_AC3
                && !stream.ac3_pause_logged) {
            stream.ac3_pause_logged = true;
            m_log.warn(eOutput, std::format("{}: audio switched away from AC3; the file's AC3 stream pauses until it is back",
                                            stream.spec.title));
        }
        return;
    }

    if (input.count == 0 || input.mode == MODE_UNKNOWN || !stream.encode)
        return;
    initResampler(stream, input.mode);

    // Compare the samples delivered so far against the video clock and
    // correct before writing, so decoding errors cannot desync the streams
    const int in_rate = audioModeSampleRate(input.mode);
    const int64_t scheduled =
            (m_video_stream.next_pts - stream.anchor_frame) * in_rate * m_video_fps_den / m_video_fps_num;
    const int64_t deficit = scheduled - (stream.in_samples + input.count);

    int skip = 0;
    if (deficit > in_rate * SYNC_MAX_STRETCH_MS / 1000) {
        insertSilence(stream, deficit, in_rate);
    } else if (deficit > in_rate * SYNC_DEAD_BAND_MS / 1000) {
        insertInterpolated(stream, deficit, in_rate, input.samples[0]);
    } else if (deficit < -in_rate * SYNC_DEAD_BAND_MS / 1000) {
        skip = (int) std::min<int64_t>(-deficit, input.count);
        m_log.warn(eOutput, std::format("{}: audio {} ms ahead of the video clock; dropping {} samples",
                                        stream.spec.title, -deficit * 1000 / in_rate, skip));
    }

    auto *q = (int16_t *) m_audio_tmp_buffer;
    for (int i = skip; i < input.count; i++)
        for (int j = 0; j < stream.in_channels; j++)
            *q++ = input.samples[i].samples[j];

    const uint8_t *in[8] = {m_audio_tmp_buffer};
    convertAndWriteAudio(stream, in, input.count - skip);
    stream.in_samples += input.count - skip;
    for (int j = 0; j < stream.in_channels; j++)
        stream.last_sample[j] = input.samples[input.count - 1].samples[j];
}

// Feed samples into the resampler and hand completed frames to the encoder.
// Called with in == nullptr (and in_samples == 0) to flush the resampler at end of stream.
void VideoFileWriter::convertAndWriteAudio(AudioStream &stream, const uint8_t **in, int in_samples) {
    OutputStream &ost = stream.ost;
    int ret = av_frame_make_writable(ost.frame);
    if (ret < 0)
        throw std::runtime_error("Failed to make audio frame writable");

    int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)ost.frame->format);

    bool filled_frame;
    do {
        uint8_t *out[8];
        if (av_sample_fmt_is_planar((AVSampleFormat)ost.frame->format)) {
            for (int i = 0; i < ost.codec_context->ch_layout.nb_channels; i++)
                out[i] = {ost.frame->data[i] + stream.samples_in_frame * bytes_per_sample};
        } else {
            out[0] = {ost.frame->data[0]
                      + stream.samples_in_frame * bytes_per_sample * ost.codec_context->ch_layout.nb_channels};
        }

        ret = swr_convert(stream.swr, out, ost.frame->nb_samples - stream.samples_in_frame, in, in_samples);
        in_samples = 0;
        if (ret < 0)
            throw std::runtime_error("Error while converting audio");

        stream.samples_in_frame += ret;

        assert (stream.samples_in_frame <= ost.frame->nb_samples);
        filled_frame = stream.samples_in_frame == ost.frame->nb_samples;
        if (filled_frame) {
            stream.encode = !writeFrame(ost.codec_context, ost.stream, ost.frame, ost.tmp_pkt);

            assert(av_frame_is_writable(ost.frame)); // it seems like audio frames are always writable after making writable the first time?

            ost.frame->pts = ost.next_pts;
            ost.next_pts += ost.frame->nb_samples;
            stream.samples_in_frame = 0;
        }
    } while (filled_frame && stream.encode);
}

// Fill a gap of `count` input samples with silence, fading out from the last
// written sample so the transition into the gap does not pop
void VideoFileWriter::insertSilence(AudioStream &stream, int64_t count, int in_rate, bool final_padding) {
    if (final_padding)
        m_log.info(eOutput, std::format("{}: padding the audio with {} ms of silence to the end of the video",
                                        stream.spec.title, count * 1000 / in_rate));
    else
        m_log.warn(eOutput, std::format("{}: audio {} ms behind the video clock; inserting silence",
                                        stream.spec.title, count * 1000 / in_rate));

    const int channels = stream.in_channels;
    const int capacity = (int) (sizeof(m_audio_tmp_buffer) / (channels * sizeof(int16_t)));
    const int64_t fade = std::min<int64_t>(64, count);

    int64_t generated = 0;
    while (generated < count) {
        int n = (int) std::min<int64_t>(count - generated, capacity);
        auto *q = (int16_t *) m_audio_tmp_buffer;
        for (int i = 0; i < n; i++, generated++)
            for (int j = 0; j < channels; j++)
                *q++ = generated < fade ? (int16_t) (stream.last_sample[j] * (fade - generated) / (fade + 1)) : 0;
        const uint8_t *in[8] = {m_audio_tmp_buffer};
        convertAndWriteAudio(stream, in, n);
    }
    for (int j = 0; j < channels; j++)
        stream.last_sample[j] = 0;
    stream.in_samples += count;
}

// Conceal a small deficit by stretching: insert `count` samples interpolated
// between the last written sample and the first sample about to be written
void VideoFileWriter::insertInterpolated(AudioStream &stream, int64_t count, int in_rate, const AudioFrame &next) {
    m_log.info(eOutput, std::format("{}: audio {} ms behind the video clock; inserting {} interpolated samples",
                                    stream.spec.title, count * 1000 / in_rate, count));

    const int channels = stream.in_channels;
    count = std::min<int64_t>(count, (int64_t) (sizeof(m_audio_tmp_buffer) / (channels * sizeof(int16_t))));

    auto *q = (int16_t *) m_audio_tmp_buffer;
    for (int64_t i = 1; i <= count; i++)
        for (int j = 0; j < channels; j++)
            *q++ = (int16_t) (stream.last_sample[j] + (next.samples[j] - stream.last_sample[j]) * i / (count + 1));
    const uint8_t *in[8] = {m_audio_tmp_buffer};
    convertAndWriteAudio(stream, in, (int) count);
    stream.in_samples += count;
}

void VideoFileWriter::cleanup() {
    writeHeader(); // a file shorter than the wait for the disc code

    for (auto &stream : m_audio) {
        if (stream.spec.ac3_passthrough)
            continue; // no encoder to pad or drain
        // Pad the audio track with silence up to the video clock, so the streams
        // end together even if audio was lost at the end of the recording
        if (stream.encode && stream.swr) {
            int in_rate = audioModeSampleRate(stream.input_mode);
            int64_t deficit = (m_video_stream.next_pts - stream.anchor_frame) * in_rate * m_video_fps_den / m_video_fps_num
                              - stream.in_samples;
            if (deficit > in_rate * SYNC_DEAD_BAND_MS / 1000)
                insertSilence(stream, deficit, in_rate, true);
        }
        // Flush pending audio: drain the resampler, then send the final (shorter) frame
        if (stream.encode && stream.swr) {
            convertAndWriteAudio(stream, nullptr, 0);
            if (stream.samples_in_frame > 0 && stream.encode) {
                stream.ost.frame->nb_samples = stream.samples_in_frame;
                stream.encode = !writeFrame(stream.ost.codec_context, stream.ost.stream, stream.ost.frame,
                                            stream.ost.tmp_pkt);
            }
        }
    }

    // Drain the encoders; without this, frames still buffered inside the
    // encoder (e.g. x264 lookahead and B frames) are lost
    if (m_encode_video)
        writeFrame(m_video_stream.codec_context, m_video_stream.stream, nullptr, m_video_stream.tmp_pkt);
    for (auto &stream : m_audio)
        if (stream.encode && !stream.spec.ac3_passthrough)
            writeFrame(stream.ost.codec_context, stream.ost.stream, nullptr, stream.ost.tmp_pkt);

    addChapters();
    av_write_trailer(m_format_context);

    for (auto &p : m_pending) // none unless the header could not be written
        av_packet_free(&p.packet);
    m_pending.clear();

    if (m_have_video)
        close_stream(&m_video_stream);
    for (auto &stream : m_audio) {
        close_stream(&stream.ost);
        swr_free(&stream.swr);
    }

    if (!(m_format_context->oformat->flags & AVFMT_NOFILE))
        avio_closep(&m_format_context->pb); // Close the output file

    avformat_free_context(m_format_context);
}

void VideoFileWriter::initAudio(AudioStream &stream, const PresetSpec &spec) {
    OutputStream &ost = stream.ost;
    initStream(&ost, spec.audio_codec);

    AVCodecContext *c = ost.codec_context;

    const AVSampleFormat *sample_formats;
    int no_sample_formats;
    int ret = avcodec_get_supported_config(
        c, ost.codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&sample_formats, &no_sample_formats);
    if (ret < 0)
        throw std::runtime_error("Could not get sample formats");
    c->sample_fmt = no_sample_formats > 0 ? sample_formats[0] : AV_SAMPLE_FMT_FLTP;

    const int *samplerates;
    int no_samplerates;
    ret = avcodec_get_supported_config(
        c, ost.codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void **)&samplerates, &no_samplerates);
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
    ost.stream->time_base = (AVRational) {1, c->sample_rate};

    ret = avcodec_open2(c, ost.codec, nullptr);
    if (ret < 0)
        throw std::runtime_error(std::format("Could not open audio codec: {}", av_err2string(ret)));

    int nb_samples = c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE ? 1024 : c->frame_size;

    ost.frame = av_frame_alloc();
    if (!ost.frame)
        throw std::runtime_error("Error allocating an audio frame");
    ost.frame->format = c->sample_fmt;
    av_channel_layout_copy(&ost.frame->ch_layout, &c->ch_layout);
    ost.frame->sample_rate = c->sample_rate;
    ost.frame->nb_samples = nb_samples;
    ost.frame->pts = 0;
    if (nb_samples) {
        if (av_frame_get_buffer(ost.frame, 0) < 0)
            throw std::runtime_error("Error allocating an audio buffer");
    }

    // copy the stream parameters to the muxer
    ret = avcodec_parameters_from_context(ost.stream->codecpar, c);
    if (ret < 0)
        throw std::runtime_error("Could not copy the stream parameters");

    // The resampler is created lazily (initResampler) once the first audio
    // arrives, since the input sample rate depends on the audio mode
}

// A stream-copy audio track for the AC3-RF bitstream: no encoder, the sync
// frames from the demodulator are muxed as they are.  The layout/bit rate
// here are the laserdisc AC3 parameters; players read the authoritative
// values from the bitstream itself.
void VideoFileWriter::initAudioPassthrough(AudioStream &stream) {
    if (avformat_query_codec(m_format_context->oformat, AV_CODEC_ID_AC3, FF_COMPLIANCE_NORMAL) != 1)
        throw std::runtime_error(std::format("AC3 cannot be stored in format {}", m_format_context->oformat->name));

    OutputStream &ost = stream.ost;
    ost.tmp_pkt = av_packet_alloc();
    if (!ost.tmp_pkt)
        throw std::runtime_error("Could not allocate AVPacket");
    ost.stream = avformat_new_stream(m_format_context, nullptr);
    if (!ost.stream)
        throw std::runtime_error("Could not allocate stream");
    ost.stream->id = (int)m_format_context->nb_streams - 1;

    AVCodecParameters *par = ost.stream->codecpar;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = AV_CODEC_ID_AC3;
    par->sample_rate = 48000;
    par->frame_size = 1536;
    par->bit_rate = 384000;
    const AVChannelLayout layout = AV_CHANNEL_LAYOUT_5POINT1;
    av_channel_layout_copy(&par->ch_layout, &layout);
    ost.stream->time_base = (AVRational){1, par->sample_rate};
}

void VideoFileWriter::writeAc3Frames(AudioStream &stream, const std::vector<std::array<uint8_t, 1536>> &ac3_frames) {
    if (ac3_frames.empty())
        return;

    // Align to the video clock like the PCM path, but a compressed stream
    // cannot be stretched: a real gap (demodulator dropout, initial lock time,
    // or the track switched away and back) becomes a jump in pts instead.
    const int64_t scheduled =
            m_video_stream.next_pts * 48000 * m_video_fps_den / m_video_fps_num;
    if (stream.ac3_next_pts < scheduled - 48000 * SYNC_MAX_STRETCH_MS / 1000) {
        m_log.warn(eOutput, std::format("{}: AC3 stream {} ms behind the video clock; leaving a gap",
                                        stream.spec.title, (scheduled - stream.ac3_next_pts) * 1000 / 48000));
        stream.ac3_next_pts = scheduled;
    }
    stream.ac3_pause_logged = false;

    for (const auto &frame : ac3_frames) {
        AVPacket *pkt = stream.ost.tmp_pkt;
        if (av_new_packet(pkt, (int)frame.size()) < 0)
            throw std::runtime_error("Could not allocate AC3 packet");
        memcpy(pkt->data, frame.data(), frame.size());
        pkt->pts = pkt->dts = stream.ac3_next_pts;
        pkt->duration = 1536;
        pkt->flags |= AV_PKT_FLAG_KEY;
        stream.ac3_next_pts += 1536;
        int ret = mux(pkt, (AVRational){1, 48000}, stream.ost.stream);
        if (ret < 0) {
            m_log.error(eOutput, std::format("Error while writing AC3 packet: {}", av_err2string(ret)));
            stream.encode = false;
            return;
        }
    }
}

void VideoFileWriter::initResampler(AudioStream &stream, AudioMode mode) {
    if (mode == stream.input_mode)
        return;

    AVCodecContext *c = stream.ost.codec_context;
    int in_sample_rate = audioModeSampleRate(mode);

    if (stream.swr) {
        m_log.warn(eOutput, std::format("{}: audio mode changed mid-recording (input now {} Hz); "
                                        "samples pending in the resampler are dropped", stream.spec.title, in_sample_rate));
        swr_free(&stream.swr);
    }

    stream.swr = swr_alloc();
    if (!stream.swr)
        throw std::runtime_error("Could not allocate resampler context");

    // The 5.1 modes deliver 6 slots in the AudioFrame 5.1 order; the resampler
    // downmixes them to the file's stereo track
    stream.in_channels = audioModeChannels(mode);
    const AVChannelLayout in_layout = stream.in_channels == MAX_AUDIO_CHANNELS
            ? (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1 : (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;

    // No asserts here: side effects in assert() disappear in NDEBUG builds
    int ret = av_opt_set_chlayout(stream.swr, "in_chlayout", &in_layout, 0);
    ret |= av_opt_set_int(stream.swr, "in_sample_rate", in_sample_rate, 0);
    ret |= av_opt_set_sample_fmt(stream.swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    ret |= av_opt_set_chlayout(stream.swr, "out_chlayout", &c->ch_layout, 0);
    ret |= av_opt_set_int(stream.swr, "out_sample_rate", c->sample_rate, 0);
    ret |= av_opt_set_sample_fmt(stream.swr, "out_sample_fmt", c->sample_fmt, 0);
    // Normalize the downmix matrix so a full-scale 5.1 input cannot clip
    ret |= av_opt_set_double(stream.swr, "rematrix_maxval", 1.0, 0);
    if (ret != 0)
        throw std::runtime_error("Failed to configure the resampling context");

    ret = swr_init(stream.swr);
    if (ret < 0)
        throw std::runtime_error("Failed to initialize the resampling context");

    // Anchor the sync schedule.  At a mode change the sample counts are in
    // different rates, so restart it at the current frame (the video frame was
    // already written, hence the -1).  On the very first init the anchor stays
    // at frame 0, so audio that starts late is padded with leading silence.
    if (stream.input_mode != MODE_UNKNOWN)
        stream.anchor_frame = m_video_stream.next_pts - 1;
    stream.in_samples = 0;

    stream.input_mode = mode;
}

void VideoFileWriter::close_stream(OutputStream *ost) {
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
        for (uint32_t y = 0; y < sizeY.y_size; y++)
            memcpy(ost->frame->data[0] + y * ost->frame->linesize[0],
                   image_Y->data<uint16_t>() + y * sizeY.x_size, sizeY.x_size * sizeof(uint16_t));

        for (uint32_t y = 0; y < sizeU.y_size; y++) {
            memcpy(ost->frame->data[1] + y * ost->frame->linesize[1],
                   image_U->data<uint16_t>() + y * sizeU.x_size, sizeU.x_size * sizeof(uint16_t));
            memcpy(ost->frame->data[2] + y * ost->frame->linesize[2],
                   image_V->data<uint16_t>() + y * sizeU.x_size, sizeU.x_size * sizeof(uint16_t));
        }
    } else {
        for (uint32_t y = 0; y < sizeY.y_size; y++)
            for (uint32_t x = 0; x < sizeY.x_size; x++)
                ost->frame->data[0][y * ost->frame->linesize[0] + x] = image_Y->data<uint16_t>()[y * sizeY.x_size + x] >> 8;

        for (uint32_t y = 0; y < sizeU.y_size; y++)
            for (uint32_t x = 0; x < sizeU.x_size; x++) {
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

        ret = mux(pkt, c->time_base, st);
        if (ret < 0)
            throw std::runtime_error(std::format("Error while writing output packet: {}", av_err2string(ret)));
    }
    return ret == AVERROR_EOF;
}
