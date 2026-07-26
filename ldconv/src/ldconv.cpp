// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
//
// ldconv - converts between the sample formats used for laserdisc RF captures.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "FileIo.h"
#include "Format.h"
#include "SampleSink.h"
#include "SampleSource.h"
#include "Transform.h"

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace {

constexpr size_t k_block_samples = 1u << 20; // 4 MiB of int32 per block
constexpr size_t k_blocks = 4;

struct Options {
    std::string input;
    std::string output;
    std::optional<Format> input_format;
    std::optional<Format> output_format;
    std::optional<double> gain;
    uint64_t start = 0;
    std::optional<uint64_t> length;
    double rate = 40e6;
    int flac_level = 8;
    unsigned flac_blocksize = 4096;
    unsigned flac_lpc_order = 0; // 0: whatever --level implies
    unsigned threads = 0;
    bool force = false;
    bool info = false;
    bool quiet = false;
};

[[noreturn]] void usage(FILE *out, int status) {
    fprintf(out,
        "ldconv - convert between laserdisc RF capture formats\n"
        "\n"
        "Usage: ldconv [options] <input> <output>\n"
        "       ldconv --info <input>\n"
        "\n"
        "Both names may be \"-\" for standard input/output, which needs an explicit\n"
        "--input-format/--output-format.  Otherwise the format comes from the file\n"
        "itself (FLAC and Ogg are recognized by their header) or from the extension.\n"
        "\n"
        "Formats: %s\n"
        "  lds        10-bit unsigned, 4 samples per 5 bytes (Domesday Duplicator)\n"
        "  r30        10-bit unsigned, 3 samples per 32-bit word\n"
        "  s8 u8      8-bit raw\n"
        "  s16 u16    16-bit raw, little endian (s16be/u16be for big endian)\n"
        "  f32        32-bit float, +-1.0 full scale (.rf)\n"
        "  flac       FLAC, as ld-compress -a writes .flac.ldf\n"
        "  ldf        FLAC in Ogg, as ld-compress -c writes .ldf and .raw.oga\n"
        "\n"
        "Options:\n"
        "      --input-format FMT   Read as FMT instead of guessing\n"
        "      --output-format FMT  Write as FMT instead of guessing\n"
        "  -g, --gain FACTOR        Sample scale factor (default: match full scale,\n"
        "                           which is x64 from 10-bit to 16-bit, as ld-decode does)\n"
        "      --no-scale           Copy code values unchanged (same as --gain 1)\n"
        "  -s, --start N            Skip N samples, or a duration such as 1.5s or 200ms\n"
        "  -n, --length N           Convert only N samples, or a duration\n"
        "  -r, --rate HZ            Capture rate for durations and for the FLAC header,\n"
        "                           which stores it in kHz (default 40e6)\n"
        "  -l, --level 0..8         FLAC compression level (default 8)\n"
        "      --blocksize N        FLAC block size (default 4096)\n"
        "      --lpc-order N        FLAC predictor order, 1..32 (default: from --level,\n"
        "                           which is 12 at level 8; 32 with --blocksize 16384\n"
        "                           is about 5%% smaller and several times slower)\n"
        "  -t, --threads N          FLAC encoder threads (default: one per core)\n"
        "  -f, --force              Overwrite the output if it exists\n"
        "      --info               Describe the input and exit\n"
        "  -q, --quiet              No progress or summary on stderr\n"
        "  -h, --help               This text\n"
        "\n"
        "Examples:\n"
        "  ldconv capture.lds capture.ldf          # compress, as ld-compress -c\n"
        "  ldconv capture.ldf capture.lds          # and back again\n"
        "  ldconv capture.ldf capture.s16          # unpack for a decoder that wants raw\n"
        "  ldconv -s 30s -n 10s in.ldf out.s16     # 10 seconds from 30 seconds in\n"
        "  ldconv --input-format s16 -r 62.5e6 - out.ldf   # from a pipe, MUSE sample rate\n",
        formatNameList().c_str());
    exit(status);
}

Format parseFormat(const std::string &name) {
    const auto format = formatFromName(name);
    if (!format)
        throw std::runtime_error(std::format("Unknown format \"{}\"; known formats are {}",
                                             name, formatNameList()));
    return *format;
}

// A plain sample count, or a duration written as 1.5s, 200ms or 500us.
uint64_t parseCount(const std::string &text, double rate) {
    char *end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (end == text.c_str() || value < 0)
        throw std::runtime_error(std::format("Not a valid count: \"{}\"", text));

    double samples;
    const std::string suffix(end);
    if (suffix.empty())      samples = value;
    else if (suffix == "s")  samples = value * rate;
    else if (suffix == "ms") samples = value * rate / 1e3;
    else if (suffix == "us") samples = value * rate / 1e6;
    else throw std::runtime_error(std::format("Not a valid count: \"{}\"", text));

    return (uint64_t)std::llround(samples);
}

std::string humanBytes(double bytes) {
    static const char *const units[] = { "B", "kB", "MB", "GB", "TB" };
    size_t unit = 0;
    while (bytes >= 1000.0 && unit + 1 < std::size(units)) {
        bytes /= 1000.0;
        unit++;
    }
    return std::format("{:.{}f} {}", bytes, unit == 0 ? 0 : 1, units[unit]);
}

std::string humanTime(double seconds) {
    if (seconds < 0 || seconds > 99 * 3600)
        return "--:--";
    const auto total = (int)std::lround(seconds);
    return std::format("{:d}:{:02d}", total / 60, total % 60);
}

bool stderrIsTerminal() {
#ifdef _WIN32
    return false;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

// Hands blocks of samples from the reading thread to the writing thread, so the
// input decode, the scaling and the output encode all overlap.
class BlockPipe {
public:
    struct Block {
        std::vector<int32_t> data;
        size_t count = 0;
    };

    BlockPipe() {
        for (size_t i = 0; i < k_blocks; i++) {
            auto block = std::make_unique<Block>();
            block->data.resize(k_block_samples);
            m_free.push(std::move(block));
        }
    }

    // nullptr when the other side has given up, so neither thread can be left
    // waiting for a block that is never coming.
    std::unique_ptr<Block> takeFree() {
        std::unique_lock lock(m_mutex);
        m_free_ready.wait(lock, [this] { return !m_free.empty() || m_aborted; });
        if (m_free.empty())
            return nullptr;
        auto block = std::move(m_free.front());
        m_free.pop();
        return block;
    }

    void putReady(std::unique_ptr<Block> block) {
        {
            std::scoped_lock lock(m_mutex);
            m_ready.push(std::move(block));
        }
        m_ready_ready.notify_one();
    }

    // nullptr once the producer has signalled the end of the input.
    std::unique_ptr<Block> takeReady() {
        std::unique_lock lock(m_mutex);
        m_ready_ready.wait(lock, [this] { return !m_ready.empty() || m_aborted; });
        if (m_ready.empty())
            return nullptr;
        auto block = std::move(m_ready.front());
        m_ready.pop();
        if (block->count == 0)
            return nullptr;
        return block;
    }

    void putFree(std::unique_ptr<Block> block) {
        {
            std::scoped_lock lock(m_mutex);
            m_free.push(std::move(block));
        }
        m_free_ready.notify_one();
    }

    void abort() {
        {
            std::scoped_lock lock(m_mutex);
            m_aborted = true;
        }
        m_free_ready.notify_all();
        m_ready_ready.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_free_ready;
    std::condition_variable m_ready_ready;
    std::queue<std::unique_ptr<Block>> m_free;
    std::queue<std::unique_ptr<Block>> m_ready;
    bool m_aborted = false;
};

Options parseArguments(int argc, char **argv) {
    Options options;
    std::vector<std::string> positional;
    std::vector<std::string> deferred; // options whose meaning depends on --rate

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
            usage(stdout, 0);

        // Accept --option=value as well as --option value.
        std::string value;
        bool has_value = false;
        if (arg.starts_with("--")) {
            const auto eq = arg.find('=');
            if (eq != std::string::npos) {
                value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                has_value = true;
            }
        }
        const auto next = [&](const char *name) {
            if (has_value)
                return value;
            if (i + 1 >= argc)
                throw std::runtime_error(std::format("{} needs a value", name));
            return std::string(argv[++i]);
        };

        if (arg == "--input-format")                      options.input_format = parseFormat(next("--input-format"));
        else if (arg == "--output-format")                options.output_format = parseFormat(next("--output-format"));
        else if (arg == "-g" || arg == "--gain")          options.gain = strtod(next("--gain").c_str(), nullptr);
        else if (arg == "--no-scale")                     options.gain = 1.0;
        else if (arg == "-r" || arg == "--rate")          options.rate = strtod(next("--rate").c_str(), nullptr);
        else if (arg == "-l" || arg == "--level")         options.flac_level = atoi(next("--level").c_str());
        else if (arg == "--blocksize")                    options.flac_blocksize = (unsigned)atoi(next("--blocksize").c_str());
        else if (arg == "--lpc-order")                    options.flac_lpc_order = (unsigned)atoi(next("--lpc-order").c_str());
        else if (arg == "-t" || arg == "--threads")       options.threads = (unsigned)atoi(next("--threads").c_str());
        else if (arg == "-f" || arg == "--force")         options.force = true;
        else if (arg == "--info")                         options.info = true;
        else if (arg == "-q" || arg == "--quiet")         options.quiet = true;
        else if (arg == "-s" || arg == "--start")         deferred.push_back("s" + next("--start"));
        else if (arg == "-n" || arg == "--length")        deferred.push_back("n" + next("--length"));
        else if (arg == "-")                              positional.push_back(arg);
        else if (arg.starts_with("-"))
            throw std::runtime_error(std::format("Unknown option \"{}\"; try --help", arg));
        else positional.push_back(arg);
    }

    // --start 1.5s means different things at different rates, so the counts are
    // resolved once the whole command line has been seen.
    for (const auto &entry : deferred) {
        const std::string text = entry.substr(1);
        if (entry[0] == 's')
            options.start = parseCount(text, options.rate);
        else
            options.length = parseCount(text, options.rate);
    }

    if (options.rate <= 0)
        throw std::runtime_error("--rate must be positive");
    if (options.flac_level < 0 || options.flac_level > 8)
        throw std::runtime_error("--level must be between 0 and 8");
    if (options.flac_lpc_order > 32)
        throw std::runtime_error("--lpc-order must be between 1 and 32");
    if (options.flac_blocksize < 16 || options.flac_blocksize > 65535)
        throw std::runtime_error("--blocksize must be between 16 and 65535");
    if (options.gain && !(*options.gain > 0))
        throw std::runtime_error("--gain must be positive");

    if (options.info) {
        if (positional.size() != 1)
            throw std::runtime_error("--info takes one input file");
        options.input = positional[0];
        return options;
    }
    if (positional.size() != 2)
        usage(stderr, 2);
    options.input = positional[0];
    options.output = positional[1];
    return options;
}

void printInfo(const Options &options) {
    auto source = makeSource(options.input, options.input_format);
    const auto &info = formatInfo(source->format());

    printf("%s: %s\n", options.input.c_str(), info.name);
    const std::string description = source->description();
    if (!description.empty())
        fputs(description.c_str(), stdout);
    printf("  Sample width: %d bit, zero level %d\n", source->bits(), source->zero());

    const uint64_t total = source->totalSamples();
    if (total != 0)
        printf("  Samples: %llu (%.3f s at %.6g MHz)\n", (unsigned long long)total,
               (double)total / options.rate, options.rate / 1e6);
    else
        printf("  Samples: unknown (the stream carries no length)\n");
}

int convert(const Options &options) {
    auto source = makeSource(options.input, options.input_format);

    Format output_format;
    if (options.output_format)
        output_format = *options.output_format;
    else if (const auto guessed = formatFromFilename(options.output))
        output_format = *guessed;
    else
        throw std::runtime_error(std::format(
            "Cannot tell what format {} should be; pass --output-format",
            options.output == "-" ? "standard output" : options.output));

    if (options.output != "-" && !options.force && std::filesystem::exists(options.output))
        throw std::runtime_error(std::format(
            "{} exists; pass --force to overwrite it", options.output));
    if (options.input != "-" && options.output != "-"
        && std::filesystem::exists(options.output)
        && std::filesystem::equivalent(options.input, options.output))
        throw std::runtime_error("The input and the output are the same file");

    // How much is there to convert?  Unknown for a pipe, and for a FLAC stream
    // whose header has no sample count.
    uint64_t to_convert = 0;
    const uint64_t available = source->totalSamples();
    if (available != 0)
        to_convert = available > options.start ? available - options.start : 0;
    if (options.length)
        to_convert = to_convert != 0 ? std::min(to_convert, *options.length) : *options.length;

    SinkOptions sink_options;
    sink_options.flac_level = options.flac_level;
    sink_options.flac_blocksize = options.flac_blocksize;
    sink_options.flac_lpc_order = options.flac_lpc_order;
    sink_options.flac_rate = (unsigned)std::clamp(std::llround(options.rate / 1e3), 1LL, 655350LL);
    sink_options.threads = options.threads;
    sink_options.total_samples = to_convert;
    auto sink = makeSink(options.output, output_format, sink_options);

    const double gain = options.gain
        ? *options.gain
        : std::ldexp(1.0, sink->bits() - source->bits());
    const auto &sink_info = formatInfo(output_format);
    Transform transform(source->zero(), sink->zero(), gain, sink_info.min_code, sink_info.max_code);

    if (!options.quiet) {
        fprintf(stderr, "%s (%s, %d bit) -> %s (%s, %d bit), gain %g\n",
                options.input.c_str(), formatInfo(source->format()).name, source->bits(),
                options.output.c_str(), sink_info.name, sink->bits(), gain);
    }

    if (options.start != 0)
        source->skip(options.start);

    const bool show_progress = !options.quiet && stderrIsTerminal();
    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;

    BlockPipe pipe;
    std::exception_ptr reader_error;
    uint64_t left = options.length ? *options.length : UINT64_MAX;

    // When the sample count is unknown, progress is measured in bytes of the
    // input file instead.  The reading thread publishes the position it has
    // reached rather than have the writing thread touch the same FILE.
    const int64_t byte_total = source->byteSize();
    const int64_t byte_start = std::max<int64_t>(source->bytePosition(), 0);
    std::atomic<int64_t> byte_position{ byte_start };

    std::thread reader([&] {
        try {
            for (;;) {
                auto block = pipe.takeFree();
                if (!block)
                    return; // the writer gave up
                const size_t want = (size_t)std::min<uint64_t>(left, block->data.size());
                block->count = want == 0 ? 0 : source->read(block->data.data(), want);
                left -= block->count;
                byte_position.store(source->bytePosition(), std::memory_order_relaxed);
                const bool last = block->count == 0;
                pipe.putReady(std::move(block));
                if (last)
                    return;
            }
        } catch (...) {
            reader_error = std::current_exception();
            // Unblock the writer with an end-of-input marker.
            if (auto block = pipe.takeFree()) {
                block->count = 0;
                pipe.putReady(std::move(block));
            }
        }
    });

    uint64_t converted = 0;
    try {
        while (auto block = pipe.takeReady()) {
            transform.apply(block->data.data(), block->count);
            sink->write(block->data.data(), block->count);
            converted += block->count;
            pipe.putFree(std::move(block));

            if (show_progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report >= std::chrono::milliseconds(250)) {
                    last_report = now;
                    const double elapsed = std::chrono::duration<double>(now - started).count();
                    const double speed = (double)converted / std::max(elapsed, 1e-9);

                    double done = -1.0; // fraction of the job, if it is knowable
                    if (to_convert != 0)
                        done = (double)std::min(converted, to_convert) / (double)to_convert;
                    else if (byte_total > byte_start) {
                        const int64_t position = byte_position.load(std::memory_order_relaxed);
                        if (position >= byte_start)
                            done = (double)(position - byte_start) / (double)(byte_total - byte_start);
                    }

                    std::string line;
                    if (done >= 0.0)
                        line = std::format("\r  {:5.1f}%  {:.1f} MS/s ({:.2f}x realtime)  eta {}  ",
                            100.0 * done, speed / 1e6, speed / options.rate,
                            humanTime(done > 0.0 ? elapsed * (1.0 - done) / done : -1.0));
                    else
                        line = std::format("\r  {} samples  {:.1f} MS/s ({:.2f}x realtime)  ",
                            converted, speed / 1e6, speed / options.rate);
                    fputs(line.c_str(), stderr);
                }
            }
        }
    } catch (...) {
        pipe.abort();
        reader.join();
        throw;
    }
    reader.join();
    if (reader_error)
        std::rethrow_exception(reader_error);

    sink->close();

    if (!options.quiet) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (show_progress)
            fputs("\r\033[K", stderr);
        const double speed = (double)converted / std::max(elapsed, 1e-9);
        fprintf(stderr, "%llu samples (%.3f s of capture) in %.1f s, %.1f MS/s (%.2fx realtime)\n",
                (unsigned long long)converted, (double)converted / options.rate,
                elapsed, speed / 1e6, speed / options.rate);
        if (options.output != "-") {
            std::error_code ec;
            const auto size = std::filesystem::file_size(options.output, ec);
            if (!ec)
                fprintf(stderr, "%s: %s\n", options.output.c_str(), humanBytes((double)size).c_str());
        }
        if (transform.clipped() != 0)
            fprintf(stderr, "warning: %llu samples clipped to the range of %s\n",
                    (unsigned long long)transform.clipped(), sink_info.name);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        const Options options = parseArguments(argc, argv);
        if (options.info) {
            printInfo(options);
            return 0;
        }
        return convert(options);
    } catch (const std::exception &e) {
        fprintf(stderr, "ldconv: %s\n", e.what());
        return 1;
    }
}
