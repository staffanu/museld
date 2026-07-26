// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>
#include "FLAC++/decoder.h"
#include "FileIo.h"
#include "SampleSource.h"

namespace {

// Everything that is a fixed number of bytes per sample group: the raw formats
// and the two packed 10-bit ones.  A group is one sample wide except for .lds
// (4 samples per 5 bytes) and .r30 (3 per 4).
class PackedSource : public SampleSource {
public:
    PackedSource(Format format, FILE *file, std::string name)
        : SampleSource(format), m_file(file), m_name(std::move(name)) {
        const auto &info = formatInfo(format);
        m_samples_per_group = info.samples_per_group;
        m_bytes_per_group = info.bytes_per_group;
        m_size = fileSize(file);
    }

    ~PackedSource() override {
        if (!isStdStream(m_file))
            fclose(m_file);
    }

    // Returns 0 only at the end of the input; a short non-zero count just means
    // the request did not divide evenly into groups, or the file ran out.
    size_t read(int32_t *out, size_t count) override {
        size_t filled = drainCarry(out, count);
        if (filled == count)
            return filled;

        const size_t groups = (count - filled) / m_samples_per_group;
        if (groups > 0) {
            m_buffer.resize(groups * m_bytes_per_group);
            const size_t got = readFull(m_file, m_buffer.data(), m_buffer.size());
            warnAboutStrayBytes(got);
            return filled + unpack(m_buffer.data(), got / m_bytes_per_group, out + filled);
        }

        // Less than a whole group is wanted, which happens on the last block of
        // a --length that does not divide evenly: decode one and keep the tail.
        m_buffer.resize(m_bytes_per_group);
        const size_t got = readFull(m_file, m_buffer.data(), m_buffer.size());
        warnAboutStrayBytes(got);
        if (got < (size_t)m_bytes_per_group)
            return filled;
        m_carry.resize((size_t)m_samples_per_group);
        m_carry_pos = 0;
        unpack(m_buffer.data(), 1, m_carry.data());
        return filled + drainCarry(out + filled, count - filled);
    }

    void skip(uint64_t count) override {
        m_carry.clear();
        m_carry_pos = 0;

        const uint64_t groups = count / m_samples_per_group;
        const uint64_t within = count % m_samples_per_group;
        if (m_size >= 0 && seekFile(m_file, (int64_t)groups * m_bytes_per_group, SEEK_CUR) == 0) {
            if (within == 0)
                return;
            // Land inside a group: decode the one it falls in and keep the tail.
            m_buffer.resize(m_bytes_per_group);
            if (readFull(m_file, m_buffer.data(), m_buffer.size()) < (size_t)m_bytes_per_group)
                return;
            std::vector<int32_t> group((size_t)m_samples_per_group);
            unpack(m_buffer.data(), 1, group.data());
            m_carry.assign(group.begin() + (ptrdiff_t)within, group.end());
            m_carry_pos = 0;
            return;
        }

        // A pipe: the only way past those samples is through them.
        std::vector<int32_t> scratch(64 * 1024);
        uint64_t left = count;
        while (left > 0) {
            const size_t want = (size_t)std::min<uint64_t>(left, scratch.size());
            const size_t got = read(scratch.data(), std::max<size_t>(want, m_samples_per_group));
            if (got == 0)
                return;
            if (got > left) {
                // Over-read within the last group; keep what was not skipped.
                m_carry.assign(scratch.begin() + (ptrdiff_t)left, scratch.begin() + (ptrdiff_t)got);
                m_carry_pos = 0;
                return;
            }
            left -= got;
        }
    }

    // The whole stream, counted from its start, before any skipping.
    uint64_t totalSamples() const override {
        if (m_size < 0)
            return 0;
        return (uint64_t)(m_size / m_bytes_per_group) * m_samples_per_group;
    }

    int64_t bytePosition() const override { return m_size < 0 ? -1 : tellFile(m_file); }
    int64_t byteSize() const override { return m_size; }

private:
    size_t drainCarry(int32_t *out, size_t count) {
        size_t filled = 0;
        while (m_carry_pos < m_carry.size() && filled < count)
            out[filled++] = m_carry[m_carry_pos++];
        return filled;
    }

    void warnAboutStrayBytes(size_t got) const {
        if (got % m_bytes_per_group != 0)
            fprintf(stderr, "warning: %s ends with %zu stray bytes, ignoring them\n",
                    m_name.c_str(), got % m_bytes_per_group);
    }

    size_t unpack(const unsigned char *in, size_t groups, int32_t *out) const {
        switch (m_format) {
            case Format::Lds:
                for (size_t g = 0; g < groups; g++, in += 5, out += 4) {
                    out[0] = (in[0] << 2) | (in[1] >> 6);
                    out[1] = ((in[1] & 0x3f) << 4) | (in[2] >> 4);
                    out[2] = ((in[2] & 0x0f) << 6) | (in[3] >> 2);
                    out[3] = ((in[3] & 0x03) << 8) | in[4];
                }
                return groups * 4;
            case Format::R30:
                for (size_t g = 0; g < groups; g++, in += 4, out += 3) {
                    const uint32_t word = (uint32_t)in[0] | ((uint32_t)in[1] << 8)
                                        | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
                    out[0] = (int32_t)(word & 0x3ff);
                    out[1] = (int32_t)((word >> 10) & 0x3ff);
                    out[2] = (int32_t)((word >> 20) & 0x3ff);
                }
                return groups * 3;
            case Format::S8:
                for (size_t i = 0; i < groups; i++)
                    out[i] = (int8_t)in[i];
                return groups;
            case Format::U8:
                for (size_t i = 0; i < groups; i++)
                    out[i] = in[i];
                return groups;
            case Format::S16:
                for (size_t i = 0; i < groups; i++)
                    out[i] = (int16_t)((uint16_t)in[2 * i] | ((uint16_t)in[2 * i + 1] << 8));
                return groups;
            case Format::U16:
                for (size_t i = 0; i < groups; i++)
                    out[i] = (uint16_t)in[2 * i] | ((uint16_t)in[2 * i + 1] << 8);
                return groups;
            case Format::S16BE:
                for (size_t i = 0; i < groups; i++)
                    out[i] = (int16_t)(((uint16_t)in[2 * i] << 8) | (uint16_t)in[2 * i + 1]);
                return groups;
            case Format::U16BE:
                for (size_t i = 0; i < groups; i++)
                    out[i] = ((uint16_t)in[2 * i] << 8) | (uint16_t)in[2 * i + 1];
                return groups;
            case Format::F32: {
                const auto &info = formatInfo(m_format);
                for (size_t i = 0; i < groups; i++) {
                    float value;
                    memcpy(&value, in + 4 * i, 4);
                    const double scaled = std::round((double)value * 32768.0);
                    out[i] = (int32_t)std::clamp(scaled, (double)info.min_code, (double)info.max_code);
                }
                return groups;
            }
            default:
                throw std::logic_error("PackedSource used for a compressed format");
        }
    }

    FILE *m_file;
    std::string m_name;
    int m_samples_per_group;
    int m_bytes_per_group;
    int64_t m_size;
    std::vector<unsigned char> m_buffer;
    std::vector<int32_t> m_carry; // samples of a partly skipped group
    size_t m_carry_pos = 0;
};

class FlacSource : public SampleSource, private FLAC::Decoder::File {
public:
    FlacSource(Format format, FILE *file, std::string name)
        : SampleSource(format), m_name(std::move(name)), m_file(file) {
        m_size = fileSize(file);
        m_seekable = m_size >= 0;
        set_md5_checking(false);

        const auto status = format == Format::OggFlac ? init_ogg(file) : init(file);
        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            if (!isStdStream(file))
                fclose(file);
            throw std::runtime_error(std::format("Cannot read {} as {}: {}", m_name,
                formatInfo(format).name, FLAC__StreamDecoderInitStatusString[status]));
        }
        m_initialized = true;

        // Run the metadata blocks now, so the stream parameters are known
        // before the first read and --info needs no samples at all.
        if (!process_until_end_of_metadata())
            throw std::runtime_error(std::format("Cannot read the header of {}: {}", m_name,
                FLAC__StreamDecoderStateString[get_state()]));
        if (m_channels != 1)
            throw std::runtime_error(std::format(
                "{} has {} channels; RF captures are mono", m_name, m_channels));
    }

    ~FlacSource() override {
        if (m_initialized) {
            m_file = nullptr; // finish() closes it, unless it is stdin
            finish();
        }
    }

    size_t read(int32_t *out, size_t count) override {
        size_t filled = 0;
        while (filled < count) {
            if (m_pending_pos == m_pending.size()) {
                if (get_state() == FLAC__STREAM_DECODER_END_OF_STREAM)
                    break;
                m_pending.clear();
                m_pending_pos = 0;
                if (!process_single())
                    throw std::runtime_error(std::format("Error decoding {}: {}", m_name,
                        FLAC__StreamDecoderStateString[get_state()]));
                alignAfterSeek();
                continue;
            }
            const size_t n = std::min(count - filled, m_pending.size() - m_pending_pos);
            memcpy(out + filled, m_pending.data() + m_pending_pos, n * sizeof(int32_t));
            m_pending_pos += n;
            filled += n;
        }
        m_position += filled;
        return filled;
    }

    void skip(uint64_t count) override {
        // Streams written through a pipe, which is how ld-compress makes them,
        // carry no sample count; libFLAC still seeks those by searching.
        if (m_seekable) {
            const uint64_t target = m_position + count;
            if (!seek_absolute(target))
                throw std::runtime_error(std::format(
                    "Cannot seek to sample {} in {}", target, m_name));
            // A seek lands on a frame boundary, and the frame it landed in has
            // already come back through write_callback: the samples before the
            // target within that frame are ours to drop.
            m_position = target;
            m_seek_target = target;
            m_seeking = true;
            alignAfterSeek();
            return;
        }
        std::vector<int32_t> scratch(64 * 1024);
        uint64_t left = count;
        while (left > 0) {
            const size_t want = (size_t)std::min<uint64_t>(left, scratch.size());
            const size_t got = read(scratch.data(), want);
            if (got == 0)
                return;
            left -= got;
        }
    }

    uint64_t totalSamples() const override { return m_total; }
    int bits() const override { return m_bits; }
    int32_t zero() const override { return 0; }

    // Ahead of the decoded position by whatever libFLAC has buffered, which is
    // close enough for a progress report.
    int64_t bytePosition() const override { return m_file == nullptr ? -1 : tellFile(m_file); }
    int64_t byteSize() const override { return m_size; }

    std::string description() const override {
        // ld-compress stores the capture rate in kHz, because FLAC cannot
        // express anything near 40 MHz; the same trick is used on the way out.
        return std::format("  FLAC stream: {} bit, {} channel, header rate {} Hz"
                           " (capture rate {:.6g} MHz if written in kHz)\n",
                           m_bits, m_channels, m_rate, m_rate / 1000.0);
    }

private:
    // Drops the part of the buffered frame that precedes the sample that was
    // seeked to.  Does nothing when no seek is outstanding.
    void alignAfterSeek() {
        if (!m_seeking || m_pending.empty())
            return;
        if (m_seek_target >= m_block_start && m_seek_target < m_block_start + m_pending.size()) {
            m_pending_pos = m_seek_target - m_block_start;
            m_seeking = false;
        } else if (m_block_start < m_seek_target) {
            m_pending.clear(); // an earlier frame; keep decoding forward
            m_pending_pos = 0;
        } else {
            m_seeking = false;  // landed past the target, which should not happen
        }
    }

    ::FLAC__StreamDecoderWriteStatus write_callback(
        const ::FLAC__Frame *frame, const FLAC__int32 *const buffer[]) override {
        // Fixed-blocksize streams number their frames rather than their
        // samples, and the final frame may be short, so the stream's own
        // blocksize is what converts one to the other.
        m_block_start = frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER
            ? frame->header.number.sample_number
            : (uint64_t)frame->header.number.frame_number
                  * (m_fixed_blocksize != 0 ? m_fixed_blocksize : frame->header.blocksize);
        m_pending.assign(buffer[0], buffer[0] + frame->header.blocksize);
        m_pending_pos = 0;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    void metadata_callback(const ::FLAC__StreamMetadata *metadata) override {
        if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
            m_bits = (int)metadata->data.stream_info.bits_per_sample;
            m_channels = (int)metadata->data.stream_info.channels;
            m_rate = metadata->data.stream_info.sample_rate;
            m_total = metadata->data.stream_info.total_samples;
            if (metadata->data.stream_info.min_blocksize == metadata->data.stream_info.max_blocksize)
                m_fixed_blocksize = metadata->data.stream_info.min_blocksize;
        }
    }

    void error_callback(::FLAC__StreamDecoderErrorStatus status) override {
        throw std::runtime_error(std::format("Error decoding {}: {}", m_name,
            FLAC__StreamDecoderErrorStatusString[status]));
    }

    std::string m_name;
    FILE *m_file;
    int64_t m_size = -1;
    bool m_initialized = false;
    bool m_seekable = false;
    int m_bits = 16;
    int m_channels = 1;
    unsigned m_rate = 0;
    unsigned m_fixed_blocksize = 0;
    uint64_t m_total = 0;
    uint64_t m_position = 0;
    uint64_t m_block_start = 0; // sample index of the buffered frame
    uint64_t m_seek_target = 0;
    bool m_seeking = false;
    std::vector<int32_t> m_pending;
    size_t m_pending_pos = 0;
};

} // namespace

std::unique_ptr<SampleSource> makeSource(const std::string &path, std::optional<Format> forced_format) {
    FILE *file = openInput(path);
    const std::string name = path == "-" ? "standard input" : path;

    Format format;
    if (forced_format) {
        format = *forced_format;
    } else {
        // The magic bytes beat the extension, because .ldf is written both as
        // Ogg FLAC (ld-compress -c) and as native FLAC (ld-compress -a).  On a
        // pipe there is nothing to peek at without consuming it, so there the
        // extension has to do.
        std::optional<Format> sniffed;
        if (fileSize(file) >= 0) {
            unsigned char magic[4];
            const size_t got = readFull(file, magic, sizeof(magic));
            seekFile(file, 0, SEEK_SET);
            sniffed = formatFromMagic(magic, got);
        }
        const auto from_name = formatFromFilename(path);
        if (sniffed && (!from_name || isFlac(*from_name)))
            format = *sniffed;
        else if (from_name)
            format = *from_name;
        else {
            if (!isStdStream(file))
                fclose(file);
            throw std::runtime_error(std::format(
                "Cannot tell what format {} is; pass --input-format", name));
        }
    }

    if (isFlac(format))
        return std::make_unique<FlacSource>(format, file, name);
    return std::make_unique<PackedSource>(format, file, name);
}
