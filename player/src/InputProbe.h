// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTPROBE_H
#define MUSECPP_INPUTPROBE_H

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <string>
#include "input/InputReader.h"

class Logger;
class PreparedInput;

// Content-based detection of what a capture holds, for museld --probe and
// --input-type auto.  Works from short windows of samples, so it is cheap even
// on multi-gigabyte captures, and needs no GPU.  A file is sampled at a few
// positions spread over its length; a live input (a fifo, a pipe on stdin,
// the network stream) is read window after window until the verdict is
// clear, so the player can be started before the disc plays.
//
// The sample format is found by magic bytes (FLAC/Ogg) or by decoding a chunk
// under each candidate format and keeping the one whose output looks like a
// band-limited signal (highest lag-1 autocorrelation); every wrong
// interpretation of packed or offset binary decorrelates adjacent samples.
// When no format correlates (the carrier high in the band, as in a 30 MHz
// capture of NTSC), every format is analysed for video structure instead,
// which takes several times longer but still finds the one that decodes.
//
// The signal type and sample rate come from the video line structure: the FM
// carrier's instantaneous frequency repeats every line, and an autocorrelation
// peak gives the line period in samples.  The period fixes the sample rate for
// each hypothesis (NTSC lines at 15.734 kHz, PAL at 15.625, MUSE at 33.75),
// and sample-rate-independent quantities decide between them: the mean carrier
// frequency in cycles per line separates MUSE (~340) from the SD standards
// (NTSC ~520, PAL ~470, overlapping), and between those the field period
// does: the lines between the vertical sync groups, 262.5 or 312.5, counted
// in line periods from where the sync tip takes over whole lines.  The
// narrowband NTSC analog audio carrier at 2.301 MHz = 146.3 cycles per line
// is the tie breaker when the field count is inconclusive.
struct InputProbeResult {
    enum class Type { eNtscRf, ePalRf, eMuseRf, eMuse16Baseband, eUnknown };

    std::optional<InputFormat> format; // detected, or the caller's echoed back
    Type type = Type::eUnknown;
    double sample_frequency = 0;       // estimated from the line rate; 0 when unknown
    bool sample_frequency_snapped = false; // estimate matched a common capture rate

    // Measurements behind the verdict, for --probe's report
    double line_period = 0;         // input samples per video line
    double line_strength = 0;       // autocorrelation at the line period, 0..1
    double cycles_per_line = 0;     // mean FM carrier cycles per video line
    double audio_carrier_ratio = 0; // narrowband PSD ratio at 146.3 cycles/line
    double field_lines = 0;         // measured lines per field (262.5 NTSC, 312.5 PAL); 0 when not measured
};

// Probes an open input.  A seekable input is left for the player to rewind;
// a live one has had whatever the detection read consumed, and carries the
// reader the detection decoded it with, which the player continues from.
// With the type given by the caller (need_type false), a live input is read
// only until the line structure is found, which is all the sample rate needs.
InputProbeResult probeInput(Logger &log, PreparedInput &input, std::optional<InputFormat> known_format,
                            bool need_type = true);

// Opens the input by name and probes it; for --probe and the tests.
InputProbeResult probeInputFile(Logger &log, const std::string &filename,
                                std::optional<InputFormat> known_format);

// Sample-rate estimate for a caller-chosen type: the measured line period times
// that type's line rate, snapped to a common capture rate when one is close.
// Works even when the probe left the type unclassified.  0 when no line
// structure was found.
double estimateSampleFrequency(const InputProbeResult &result, InputProbeResult::Type type);

// Watches a live input while it plays for a change of type (a disc of another
// standard put in the player).  The demodulator feeds it the samples it reads;
// every few seconds of input -- every second while the reader has no sync --
// it collects a chunk long enough for the field-period test and classifies it
// on a thread of its own, exactly as the probe would, and once three
// consecutive verdicts agree on a type other than the current one, otherType()
// reports it.  A chunk that arrives while the previous one is still being
// analysed is skipped: the demodulator is never held up.
class ContentWatch {
public:
    using Type = InputProbeResult::Type;
    ContentWatch(Logger &log, Type current, double sample_rate);
    ~ContentWatch();
    ContentWatch(const ContentWatch &) = delete;
    ContentWatch &operator=(const ContentWatch &) = delete;

    // The demodulator thread, with each block it reads; `urgent` while the
    // reader has no sync
    void feed(const float *samples, size_t n, bool urgent);
    // The other type, once decided; never cleared
    std::optional<Type> otherType() const;

private:
    void worker();
    void classify(const std::vector<float> &chunk);

    Logger &m_log;
    const Type m_current;
    const double m_sample_rate;
    const size_t m_chunk_samples;
    const int64_t m_interval_samples;        // between analyses, in input samples
    const int64_t m_urgent_interval_samples;
    int64_t m_since_analysis;
    std::vector<float> m_chunk;              // being collected (demodulator thread)
    bool m_collecting;
    std::vector<float> m_pending;            // handed to the worker
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop;
    Type m_candidate;
    int m_agreeing;
    std::atomic<int> m_other;                // -1, or the other Type
    std::thread m_thread;                    // last: joined before the state above goes
};

#endif //MUSECPP_INPUTPROBE_H
