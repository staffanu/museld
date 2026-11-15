// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <thread>
#include <vector>
#include "FLAC++/encoder.h"
#include "FileIo.h"
#include "SampleSink.h"

namespace {

class PackedSink : public SampleSink {
public:
    PackedSink(Format format, FILE *file, std::string name)
        : SampleSink(format), m_file(file), m_name(std::move(name)) {
        const auto &info = formatInfo(format);
        m_samples_per_group = info.samples_per_group;
        m_bytes_per_group = info.bytes_per_group;
    }

    ~PackedSink() override {
        if (m_file != nullptr && !isStdStream(m_file))
            fclose(m_file);
    }

    void write(const int32_t *in, size_t count) override {
        if (m_samples_per_group == 1) {
            packAndWrite(in, count);
            return;
        }

        // Complete the group left over from the previous block first.
        if (!m_carry.empty()) {
            while (count > 0 && m_carry.size() < (size_t)m_samples_per_group) {
                m_carry.push_back(*in++);
                count--;
            }
            if (m_carry.size() < (size_t)m_samples_per_group)
                return;
            packAndWrite(m_carry.data(), m_carry.size());
            m_carry.clear();
        }

        const size_t whole = count - count % m_samples_per_group;
        packAndWrite(in, whole);
        m_carry.assign(in + whole, in + count);
    }

    void close() override {
        if (!m_carry.empty()) {
            // The packed formats have no way to store a fraction of a group,
            // so the tail is padded to the zero level rather than dropped.
            const auto &info = formatInfo(m_format);
            fprintf(stderr, "warning: padding the last %d samples of %s to a whole group\n",
                    m_samples_per_group - (int)m_carry.size(), m_name.c_str());
            m_carry.resize(m_samples_per_group, info.zero);
            packAndWrite(m_carry.data(), m_carry.size());
            m_carry.clear();
        }
        if (fflush(m_file) != 0)
            throw std::runtime_error(std::format("Cannot flush {}", m_name));
        if (!isStdStream(m_file)) {
            if (fclose(m_file) != 0)
                throw std::runtime_error(std::format("Cannot close {}", m_name));
        }
        m_file = nullptr;
    }

private:
    void packAndWrite(const int32_t *in, size_t count) {
        if (count == 0)
            return;
        const size_t groups = count / m_samples_per_group;
        m_buffer.resize(groups * m_bytes_per_group);
        unsigned char *out = m_buffer.data();

        switch (m_format) {
            case Format::Lds:
                for (size_t g = 0; g < groups; g++, in += 4, out += 5) {
                    out[0] = (unsigned char)(in[0] >> 2);
                    out[1] = (unsigned char)(((in[0] & 0x003) << 6) | ((in[1] & 0x3f0) >> 4));
                    out[2] = (unsigned char)(((in[1] & 0x00f) << 4) | ((in[2] & 0x3c0) >> 6));
                    out[3] = (unsigned char)(((in[2] & 0x03f) << 2) | ((in[3] & 0x300) >> 8));
                    out[4] = (unsigned char)(in[3] & 0x0ff);
                }
                break;
            case Format::R30:
                for (size_t g = 0; g < groups; g++, in += 3, out += 4) {
                    const uint32_t word = (uint32_t)(in[0] & 0x3ff)
                                        | ((uint32_t)(in[1] & 0x3ff) << 10)
                                        | ((uint32_t)(in[2] & 0x3ff) << 20);
                    out[0] = (unsigned char)(word & 0xff);
                    out[1] = (unsigned char)((word >> 8) & 0xff);
                    out[2] = (unsigned char)((word >> 16) & 0xff);
                    out[3] = (unsigned char)((word >> 24) & 0xff);
                }
                break;
            case Format::S8:
            case Format::U8:
                for (size_t i = 0; i < groups; i++)
                    out[i] = (unsigned char)(in[i] & 0xff);
                break;
            case Format::S16:
            case Format::U16:
                for (size_t i = 0; i < groups; i++) {
                    out[2 * i]     = (unsigned char)(in[i] & 0xff);
                    out[2 * i + 1] = (unsigned char)((in[i] >> 8) & 0xff);
                }
                break;
            case Format::S16BE:
            case Format::U16BE:
                for (size_t i = 0; i < groups; i++) {
                    out[2 * i]     = (unsigned char)((in[i] >> 8) & 0xff);
                    out[2 * i + 1] = (unsigned char)(in[i] & 0xff);
                }
                break;
            case Format::F32:
                for (size_t i = 0; i < groups; i++) {
                    const float value = (float)in[i] / 32768.0f;
                    memcpy(out + 4 * i, &value, 4);
                }
                break;
            default:
                throw std::logic_error("PackedSink used for a compressed format");
        }

        writeFull(m_file, m_buffer.data(), m_buffer.size());
    }

    FILE *m_file;
    std::string m_name;
    int m_samples_per_group;
    int m_bytes_per_group;
    std::vector<unsigned char> m_buffer;
    std::vector<int32_t> m_carry; // samples of an incomplete trailing group
};

// The Ogg page checksum: CRC-32 with the 0x04C11DB7 polynomial, no reflection,
// zero initial value, over the page with its CRC field zeroed
uint32_t oggCrc(const unsigned char *p, size_t n) {
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

// STREAMINFO holds the sample count in 36 bits.  libFLAC's plain FLAC encoder writes
// 0 ("unknown") for a count that does not fit, but its Ogg encoder (1.5.0) writes the
// count masked to the field, and decoders trusting the wrapped value refuse every
// seek past it.  So an Ogg file written with that many samples gets the field
// cleared here, in the first page, whose checksum is rewritten.  The length is in
// the last page's granule position anyway (see FlacSource).
constexpr uint64_t k_streaminfo_total_limit = 1ULL << 36;

uint64_t streamInfoTotalLimit() {
    // For the tests, which cannot encode 2^36 samples
    if (const char *env = getenv("LDCONV_STREAMINFO_TOTAL_LIMIT"); env != nullptr && *env != '\0')
        return strtoull(env, nullptr, 10);
    return k_streaminfo_total_limit;
}

void clearOggStreamInfoTotal(const std::string &path) {
    constexpr size_t k_page = 27 + 1 + 9 + 4 + 4 + 34; // header, one lacing byte, the FLAC-in-Ogg packet
    constexpr size_t k_total = 28 + 17 + 13;          // the field: low nibble of this byte and the next four
    FILE *file = fopen(path.c_str(), "r+b");
    if (file == nullptr)
        throw std::runtime_error(std::format("Cannot reopen {} to clear its sample count: {}", path, strerror(errno)));
    unsigned char page[k_page];
    const bool ok = readFull(file, page, k_page) == k_page && memcmp(page, "OggS", 4) == 0 && page[26] == 1
        && page[27] == 51 && page[28] == 0x7F && memcmp(page + 29, "FLAC", 4) == 0 && memcmp(page + 37, "fLaC", 4) == 0;
    if (ok) {
        page[k_total] &= 0xF0;
        memset(page + k_total + 1, 0, 4);
        memset(page + 22, 0, 4);
        const uint32_t crc = oggCrc(page, k_page);
        for (int k = 0; k < 4; k++)
            page[22 + k] = (unsigned char)(crc >> (8 * k));
        if (seekFile(file, 0, SEEK_SET) == 0)
            writeFull(file, page, k_page);
    }
    fclose(file);
    if (!ok)
        throw std::runtime_error(std::format("{} does not start with the FLAC-in-Ogg page expected", path));
}

class FlacSink : public SampleSink, private FLAC::Encoder::File {
public:
    FlacSink(Format format, FILE *file, std::string name, const SinkOptions &options)
        : SampleSink(format), m_name(std::move(name)), m_ogg(format == Format::OggFlac), m_to_file(!isStdStream(file)) {
        set_channels(1);
        set_bits_per_sample(16);
        set_sample_rate(options.flac_rate);
        // Both of these have to come after the compression level, which sets
        // them itself.
        set_compression_level((uint32_t)options.flac_level);
        set_blocksize(options.flac_blocksize);
        if (options.flac_lpc_order != 0)
            set_max_lpc_order(options.flac_lpc_order);
        set_verify(false);
        // RF captures are not music: nothing downstream needs a subset stream,
        // and insisting on one only rules out the larger block sizes.
        set_streamable_subset(false);
        // STREAMINFO stores the total in 36 bits.  A longer capture must
        // declare "unknown" (0): libFLAC masks the estimate to the field
        // width, and decoders trust the wrapped result -- libFLAC refuses
        // every seek past it, which broke museld's --probe on long captures.
        if (options.total_samples != 0 && options.total_samples < (1ULL << 36))
            set_total_samples_estimate(options.total_samples);

#if FLAC_API_VERSION_CURRENT >= 14
        unsigned threads = options.threads;
        if (threads == 0)
            threads = std::max(1u, std::thread::hardware_concurrency());
        for (; threads > 1; threads--) {
            const uint32_t status = set_num_threads(threads);
            if (status == FLAC__STREAM_ENCODER_SET_NUM_THREADS_OK)
                break;
            // Only an over-large count is worth retrying; the other refusals
            // (no multithreading in this build) will not change.
            if (status != FLAC__STREAM_ENCODER_SET_NUM_THREADS_TOO_MANY_THREADS)
                break;
        }
        m_threads = get_num_threads();
#endif

        const auto status = format == Format::OggFlac ? init_ogg(file) : init(file);
        if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            if (!isStdStream(file))
                fclose(file);
            throw std::runtime_error(std::format("Cannot write {} as {}: {}", m_name,
                formatInfo(format).name, FLAC__StreamEncoderInitStatusString[status]));
        }
        m_initialized = true;
    }

    ~FlacSink() override {
        if (m_initialized)
            FLAC::Encoder::File::finish();
    }

    void write(const int32_t *in, size_t count) override {
        // FLAC__int32 is int32_t, so the block goes to the encoder as it is.
        const FLAC__int32 *const channels[1] = { in };
        size_t done = 0;
        while (done < count) {
            // process() takes a 32-bit sample count.
            const auto n = (uint32_t)std::min<size_t>(count - done, 1u << 24);
            const FLAC__int32 *const block[1] = { channels[0] + done };
            if (!process(block, n))
                throw std::runtime_error(std::format("Error encoding {}: {}", m_name,
                    FLAC__StreamEncoderStateString[get_state()]));
            done += n;
        }
        m_samples_written += count;
    }

    void close() override {
        if (!m_initialized)
            return;
        m_initialized = false;
        if (!FLAC::Encoder::File::finish())
            throw std::runtime_error(std::format("Error finishing {}: {}", m_name,
                FLAC__StreamEncoderStateString[get_state()]));
        // finish() wrote the count into STREAMINFO; see clearOggStreamInfoTotal
        if (m_ogg && m_to_file && m_samples_written >= streamInfoTotalLimit())
            clearOggStreamInfoTotal(m_name);
    }

    unsigned threads() const { return m_threads; }

private:
    std::string m_name;
    bool m_ogg;
    bool m_to_file; // a path that can be reopened, not the standard output
    bool m_initialized = false;
    unsigned m_threads = 1;
    uint64_t m_samples_written = 0;
};

} // namespace

std::unique_ptr<SampleSink> makeSink(const std::string &path, Format format, const SinkOptions &options) {
    FILE *file = openOutput(path);
    const std::string name = path == "-" ? "standard output" : path;

    if (isFlac(format))
        return std::make_unique<FlacSink>(format, file, name, options);
    return std::make_unique<PackedSink>(format, file, name);
}
