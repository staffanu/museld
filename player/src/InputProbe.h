// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTPROBE_H
#define MUSECPP_INPUTPROBE_H

#include <cstdint>
#include <optional>
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

#endif //MUSECPP_INPUTPROBE_H
