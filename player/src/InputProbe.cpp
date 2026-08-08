// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <valarray>
#include <vector>
#include "InputProbe.h"
#include "input/InputReaderFactory.h"
#include "filter/WindowedSinc.h"
#include "filter/FFT.h"
#include "logging/Logger.h"

using namespace std;

namespace {

constexpr uint32_t c_chunk_samples = 1 << 21;
constexpr int c_block_average = 16;      // instantaneous-frequency decimation
constexpr int c_min_lag = 30;            // line-period search range, in averaged blocks:
constexpr int c_max_lag = 400;           // 480..6400 samples per line
constexpr int c_analytic_taps = 129;
// High-pass edge of the analytic filter, as a fraction of the sample rate.
// Kills EFM and the analog audio carriers (all below 2.9 MHz at any realistic
// capture rate) so the instantaneous frequency tracks the video FM carrier.
constexpr double c_highpass_fraction = 0.06;

constexpr double c_ntsc_line_hz = 15734.2657;
constexpr double c_muse_line_hz = 33750.0;
// The NTSC left analog audio carrier, in carrier cycles per video line
// (2.301 MHz / line rate).  Present on virtually every NTSC laserdisc (AC3
// discs drop only the right carrier) and absent from MUSE.
constexpr double c_ntsc_audio_cycles_per_line = 2.301e6 / c_ntsc_line_hz;

// Common capture rates to snap the line-rate estimate to (DdD, cxadc, FX3).
constexpr double c_known_rates[] = {28.63636e6, 35.79545e6, 40e6, 46.08e6, 50e6, 62.5e6, 64e6, 80e6, 100e6};

struct ChunkStats {
    double line_period;         // samples
    double line_strength;       // normalized autocorrelation at the peak
    double cycles_per_line;
    double audio_carrier_ratio;
    double muse16_significance; // fold-480 line profile height over the noise floor
    double muse16_ratio;        // ...relative to off-period control folds
};

// Read chunk_samples floats starting at the given sample offset with a fresh
// reader, so probing never disturbs playback state.  Returns an empty vector
// when the file does not reach that far.
vector<float> readChunk(const string &filename, InputFormat format,
                        int64_t sample_offset, uint32_t chunk_samples) {
    vector<float> samples(chunk_samples);
    try {
        auto reader = makeInputReader(filename, format, chunk_samples);
        reader->initialize();
        if (sample_offset > 0)
            reader->seek(sample_offset);
        if (reader->readFloats(samples.data()) != (int)chunk_samples)
            return {};
    } catch (const std::exception &) {
        return {}; // unreadable at this offset (e.g. a FLAC seek past the end)
    }
    return samples;
}

double lag1Autocorrelation(const vector<float> &x) {
    double mean = 0;
    for (float v : x) mean += v;
    mean /= (double)x.size();
    double denom = 0, num = 0;
    double prev = x[0] - mean;
    denom += prev * prev;
    for (size_t i = 1; i < x.size(); i++) {
        double v = x[i] - mean;
        num += v * prev;
        denom += v * v;
        prev = v;
    }
    return denom > 0 ? num / denom : 0;
}

optional<InputFormat> detectFormatByMagic(const string &filename) {
    ifstream f(filename, ios::binary);
    char magic[4] = {};
    f.read(magic, 4);
    if (memcmp(magic, "fLaC", 4) == 0)
        return eFlac;
    if (memcmp(magic, "OggS", 4) == 0)
        return eFlacOgg;
    return nullopt;
}

// Formats the file's bytes could plausibly be in, best first.  A real capture
// read under its true format is heavily oversampled and strongly correlated
// sample to sample; a wrong guess scrambles bits or signs and decorrelates.
// Closeness is judged on the decorrelation (1 - score), because on a very
// smooth capture the wrong interpretations also score high in absolute terms
// (a u8 file read as u16 still pairs correlated bytes) and only the distance
// to perfect correlation separates them.  When more than one candidate
// survives, the caller breaks the tie by which one shows video line structure.
vector<InputFormat> rawFormatCandidates(Logger &log, const string &filename) {
    constexpr InputFormat formats[] = {eUint8, eSint8, eUint16, eSint16, eUint16BE, eSint16BE, eLds};
    constexpr uint32_t score_samples = 1 << 20;
    vector<pair<double, InputFormat>> scored;
    for (InputFormat fmt : formats) {
        // Score away from the file start, where lead-in may not look like RF,
        // but fall back to the start for short files
        auto samples = readChunk(filename, fmt, 8 << 20, score_samples);
        if (samples.empty())
            samples = readChunk(filename, fmt, 0, score_samples);
        if (samples.empty())
            continue;
        double score = lag1Autocorrelation(samples);
        log.debug(eInput, std::format("Probe: format {} lag-1 autocorrelation {:+.4f}",
                                      inputFormatName(fmt), score));
        if (score > 0.15)
            scored.emplace_back(score, fmt);
    }
    sort(scored.begin(), scored.end(), [](auto &a, auto &b) { return a.first > b.first; });
    vector<InputFormat> candidates;
    for (const auto &[score, fmt] : scored)
        if (candidates.size() < 3 && (1 - scored.front().first) > 0.5 * (1 - score))
            candidates.push_back(fmt);
    return candidates;
}

// Analytic signal of the high-passed input; the taps are designed at Fs = 1 so
// the band edges scale with whatever the true sample rate is.
vector<complex<float>> analyticSignal(const vector<float> &x) {
    // The loop below correlates rather than convolves, so reverse the taps
    // (same convention as the demodulator FIR shaders); without this the
    // filter passes the negative-frequency band instead
    static const vector<complex<float>> taps = [] {
        auto t = WindowedSinc::complex_band_pass<float>(c_analytic_taps, 1.0, c_highpass_fraction, 0.47);
        reverse(t.begin(), t.end());
        return t;
    }();
    const size_t n = x.size() - taps.size() + 1;
    vector<complex<float>> out(n);
    for (size_t i = 0; i < n; i++) {
        float re = 0, im = 0;
        for (size_t k = 0; k < taps.size(); k++) {
            re += x[i + k] * taps[k].real();
            im += x[i + k] * taps[k].imag();
        }
        out[i] = {re, im};
    }
    return out;
}

// Instantaneous frequency in cycles/sample, and its energy-weighted mean
pair<vector<float>, double> instantaneousFrequency(const vector<complex<float>> &a) {
    vector<float> freq(a.size() - 1);
    double weighted_sum = 0, weight = 0;
    for (size_t i = 0; i + 1 < a.size(); i++) {
        complex<float> r = a[i + 1] * conj(a[i]);
        float f = atan2f(r.imag(), r.real()) * (float)(0.5 / M_PI);
        freq[i] = f;
        double w = norm(a[i]);
        weighted_sum += f * w;
        weight += w;
    }
    return {std::move(freq), weight > 0 ? weighted_sum / weight : 0};
}

vector<float> blockAverage(const vector<float> &x, int w) {
    vector<float> out(x.size() / w);
    for (size_t i = 0; i < out.size(); i++) {
        float sum = 0;
        for (int k = 0; k < w; k++)
            sum += x[i * w + k];
        out[i] = sum / (float)w;
    }
    return out;
}

// Normalized autocorrelation of d at each lag in [c_min_lag, c_max_lag)
vector<double> autocorrelation(const vector<float> &d) {
    double mean = 0;
    for (float v : d) mean += v;
    mean /= (double)d.size();
    vector<float> c(d.size());
    double denom = 0;
    for (size_t i = 0; i < d.size(); i++) {
        c[i] = d[i] - (float)mean;
        denom += (double)c[i] * c[i];
    }
    vector<double> ac(c_max_lag - c_min_lag);
    for (int lag = c_min_lag; lag < c_max_lag; lag++) {
        double sum = 0;
        for (size_t i = lag; i < c.size(); i++)
            sum += (double)c[i] * c[i - lag];
        ac[lag - c_min_lag] = sum / denom;
    }
    return ac;
}

// Line period in samples from the autocorrelation peak.  The strongest peak
// can be a harmonic (two or three lines), so prefer a near-as-strong peak at
// an integer fraction of its lag, then refine with a parabolic fit.
pair<double, double> findLinePeriod(const vector<double> &ac) {
    int best = (int)(max_element(ac.begin(), ac.end()) - ac.begin());
    for (int divisor : {3, 2}) {
        double target = (double)(best + c_min_lag) / divisor - c_min_lag;
        int j = (int)lround(target);
        for (int cand : {j - 1, j, j + 1})
            if (cand >= 1 && cand + 1 < (int)ac.size() && ac[cand] > 0.72 * ac[best] &&
                ac[cand] >= ac[cand - 1] && ac[cand] >= ac[cand + 1]) {
                best = cand;
                break;
            }
        if (best == j || best == j - 1 || best == j + 1)
            break;
    }
    double refined = best;
    if (best >= 1 && best + 1 < (int)ac.size()) {
        double num = ac[best - 1] - ac[best + 1];
        double den = 2 * (ac[best - 1] - 2 * ac[best] + ac[best + 1]);
        if (den != 0)
            refined += num / den;
    }
    return {(refined + c_min_lag) * c_block_average, ac[best]};
}

// Ratio of the strongest narrowband PSD component near the expected NTSC audio
// carrier position (in cycles/line) to the local background level
double audioCarrierRatio(const vector<float> &x, double line_period) {
    constexpr int nfft = 1 << 15;
    constexpr int max_segments = 24;
    vector<double> psd(nfft / 2 + 1, 0.0);
    int segments = min((int)(x.size() / nfft), max_segments);
    if (segments == 0)
        return 0;
    valarray<complex<float>> seg(nfft);
    for (int s = 0; s < segments; s++) {
        for (int i = 0; i < nfft; i++) {
            float window = 0.5f - 0.5f * cosf((float)(2.0 * M_PI * i / nfft));
            seg[i] = x[(size_t)s * nfft + i] * window;
        }
        FFT<float>::fft(seg);
        for (size_t i = 0; i < psd.size(); i++)
            psd[i] += norm(seg[i]);
    }
    // Bin k is at k/nfft cycles/sample = k*line_period/nfft cycles/line
    auto bin = [&](double cycles_per_line) {
        return (int)lround(cycles_per_line * nfft / line_period);
    };
    int lo = bin(c_ntsc_audio_cycles_per_line - 1.5), hi = bin(c_ntsc_audio_cycles_per_line + 1.5);
    int bg_lo = bin(c_ntsc_audio_cycles_per_line - 12), bg_hi = bin(c_ntsc_audio_cycles_per_line + 12);
    if (bg_lo < 0 || bg_hi >= (int)psd.size())
        return 0;
    double peak = *max_element(psd.begin() + lo, psd.begin() + hi + 1);
    vector<double> background;
    for (int i = bg_lo; i <= bg_hi; i++)
        if (i < lo || i > hi)
            background.push_back(psd[i]);
    nth_element(background.begin(), background.begin() + background.size() / 2, background.end());
    double median = background[background.size() / 2];
    return median > 0 ? peak / median : 0;
}

// Line-locked structure at an exact fold size, as the height of the folded
// mean profile over its averaging noise floor.  MUSE baseband needs this
// rather than autocorrelation: the dot-interleaved subsampling decorrelates
// adjacent lines, so only averaging thousands of lines shows the tiny sync.
double foldSignificance(const vector<float> &x, int period) {
    constexpr size_t k = 32; // high-pass, so slow drift does not inflate the profile
    vector<float> hp(x.size());
    double running = 0;
    for (size_t i = 0; i < x.size(); i++) {
        running += x[i];
        if (i >= k)
            running -= x[i - k];
        hp[i] = x[i] - (float)(running / (double)min(i + 1, k));
    }
    const size_t lines = hp.size() / period;
    if (lines < 100)
        return 0;
    vector<double> profile(period, 0.0);
    double energy = 0;
    for (size_t l = 0; l < lines; l++)
        for (int i = 0; i < period; i++) {
            double v = hp[l * period + i];
            profile[i] += v;
            energy += v * v;
        }
    double mean = 0;
    for (double &p : profile) {
        p /= (double)lines;
        mean += p;
    }
    mean /= period;
    double profile_var = 0;
    for (double p : profile)
        profile_var += (p - mean) * (p - mean);
    profile_var /= period;
    double noise_var = energy / ((double)lines * period) / (double)lines;
    return noise_var > 0 ? sqrt(profile_var / noise_var) : 0;
}

ChunkStats analyzeChunk(Logger &log, const vector<float> &samples, int64_t offset) {
    auto analytic = analyticSignal(samples);
    auto [freq, mean_carrier] = instantaneousFrequency(analytic);
    auto averaged = blockAverage(freq, c_block_average);
    auto ac = autocorrelation(averaged);
    auto [period, strength] = findLinePeriod(ac);
    ChunkStats stats{period, strength, mean_carrier * period, audioCarrierRatio(samples, period), 0, 0};
    stats.muse16_significance = foldSignificance(samples, 480);
    double control = max(foldSignificance(samples, 455), foldSignificance(samples, 509));
    stats.muse16_ratio = control > 0 ? stats.muse16_significance / control : 0;
    log.debug(eInput, std::format(
            "Probe: chunk at sample {}: line period {:.1f} (autocorrelation {:.3f}), "
            "{:.1f} carrier cycles/line, audio carrier ratio {:.1f}, "
            "fold-480 significance {:.1f} ({:.1f}x control)",
            offset, stats.line_period, stats.line_strength, stats.cycles_per_line,
            stats.audio_carrier_ratio, stats.muse16_significance, stats.muse16_ratio));
    return stats;
}

double median(vector<double> v) {
    nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Chunk positions spread over the file, from the sample count implied by the
// byte size.  For FLAC the count is not knowable without decoding; bytes/2 is
// a safe underestimate (16-bit sources always compress below 2 bytes/sample),
// so the offsets stay inside the file but reach past any lead-in.
double formatBytesPerSample(InputFormat format) {
    switch (format) {
        case eUint8: case eSint8: return 1;
        case eLds:                return 1.25;
        default:                  return 2; // 16-bit raw, and FLAC's usual source width
    }
}

vector<int64_t> chunkOffsets(const string &filename, InputFormat format) {
    double bytes_per_sample = formatBytesPerSample(format);
    error_code ec;
    int64_t total = (int64_t)((double)filesystem::file_size(filename, ec) / bytes_per_sample);
    if (ec || total < (int64_t)c_chunk_samples)
        return {0};
    vector<int64_t> offsets;
    for (double frac : {0.08, 0.45, 0.75}) {
        int64_t off = min((int64_t)(frac * (double)total), total - (int64_t)c_chunk_samples);
        offsets.push_back(max<int64_t>(0, off));
    }
    return offsets;
}

// The per-file measurements under one candidate format: chunk analysis plus
// cross-chunk reconciliation.  consensus is false when no majority of chunks
// agrees on a line period (the measurement fields are then zero).
struct FileAnalysis {
    bool consensus = false;
    double line_period = 0;
    double line_strength = 0;
    double cycles_per_line = 0;
    double audio_carrier_ratio = 0;
    double muse16_significance = 0;
    double muse16_ratio = 0;

    // The fold-480 test carries muse-16 on its own; the line-period consensus
    // carries everything else
    bool isMuse16() const { return muse16_significance > 5 && muse16_ratio > 3; }
};

FileAnalysis analyzeFile(Logger &log, const string &filename, InputFormat format) {
    FileAnalysis analysis;
    vector<ChunkStats> chunks;
    vector<double> fold_significances, fold_ratios;
    for (int64_t offset : chunkOffsets(filename, format)) {
        auto samples = readChunk(filename, format, offset, c_chunk_samples);
        if (samples.empty())
            continue;
        ChunkStats stats = analyzeChunk(log, samples, offset);
        fold_significances.push_back(stats.muse16_significance);
        fold_ratios.push_back(stats.muse16_ratio);
        if (stats.line_strength >= 0.25) // convincing line structure only
            chunks.push_back(stats);
    }
    if (!fold_significances.empty()) {
        analysis.muse16_significance = median(fold_significances);
        analysis.muse16_ratio = median(fold_ratios);
    }
    if (chunks.empty())
        return analysis;

    // A chunk in the lead-in or a dropout can lock onto something that is not
    // the line structure, and a clean chunk can lock two or three lines apart
    // (a harmonic of the period).  Pick the reference chunk most others agree
    // with -- counting a chunk as agreeing when an integer division of its
    // period lands on the reference -- and require a majority so a single
    // rogue chunk cannot decide.
    auto harmonicOf = [](double period, double reference) -> int {
        int k = (int)lround(period / reference);
        return k >= 1 && abs(period / k - reference) / reference <= 0.03 ? k : 0;
    };
    size_t best_ref = 0, best_count = 0;
    for (size_t i = 0; i < chunks.size(); i++) {
        size_t count = 0;
        for (const auto &c : chunks)
            count += harmonicOf(c.line_period, chunks[i].line_period) != 0;
        if (count > best_count ||
            (count == best_count && chunks[i].line_strength > chunks[best_ref].line_strength)) {
            best_ref = i;
            best_count = count;
        }
    }
    if (best_count * 2 <= chunks.size())
        return analysis;

    vector<double> periods, cycles, strengths;
    for (const auto &c : chunks) {
        int k = harmonicOf(c.line_period, chunks[best_ref].line_period);
        if (k == 0)
            continue;
        periods.push_back(c.line_period / k);
        cycles.push_back(c.cycles_per_line / k);
        strengths.push_back(c.line_strength);
        // A harmonic lock measured the audio carrier at the wrong position
        if (k == 1)
            analysis.audio_carrier_ratio = max(analysis.audio_carrier_ratio, c.audio_carrier_ratio);
    }
    analysis.consensus = true;
    analysis.line_period = median(periods);
    analysis.line_strength = median(strengths);
    analysis.cycles_per_line = median(cycles);
    return analysis;
}

} // namespace

InputProbeResult probeInputFile(Logger &log, const string &filename,
                                optional<InputFormat> known_format) {
    InputProbeResult result;

    result.format = known_format ? known_format : detectFormatByMagic(filename);
    vector<InputFormat> candidates;
    if (result.format)
        candidates = {*result.format};
    else
        candidates = rawFormatCandidates(log, filename);
    if (candidates.empty())
        return result;

    // Analyze under each candidate format and let the video structure break
    // byte-level ties: only the true format decodes into a video signal (a
    // smooth baseband u16 capture scores high as u8 too, but shows no line
    // periodicity that way).  Format width breaks ties asymmetrically: a true
    // 8-bit capture read as 16-bit pairs adjacent smooth samples and still
    // looks like a signal (at half the rate -- it can even show line
    // structure), while a true 16-bit capture read as 8-bit interleaves noisy
    // low bytes with slow high bytes and cannot look good.  So a narrower
    // format that shows structure is the truth, no matter how well a wider
    // one scores.  Among equal widths, a muse-16 fold hit outranks an RF line
    // lock, since the fold is the far more specific signature.
    FileAnalysis best{};
    bool have_best = false;
    auto quality = [](const FileAnalysis &a) {
        return a.isMuse16() ? 100.0 + a.muse16_significance
                            : (a.consensus ? a.line_strength : 0.0);
    };
    for (InputFormat fmt : candidates) {
        FileAnalysis a = analyzeFile(log, filename, fmt);
        if (quality(a) <= 0)
            continue;
        bool narrower = have_best &&
                formatBytesPerSample(fmt) < formatBytesPerSample(*result.format);
        bool wider = have_best &&
                formatBytesPerSample(fmt) > formatBytesPerSample(*result.format);
        if (!have_best || narrower || (!wider && quality(a) > quality(best))) {
            best = a;
            result.format = fmt;
            have_best = true;
        }
    }
    if (!have_best) {
        // No format shows video structure.  Report the byte-level winner if it
        // was unambiguous, or -- by the same asymmetry as above -- if the top
        // scorer is strictly narrower than every other candidate: the wider
        // readings correlating too is then expected, the reverse impossible.
        bool narrowest = true;
        for (size_t i = 1; i < candidates.size(); i++)
            narrowest &= formatBytesPerSample(candidates.front()) < formatBytesPerSample(candidates[i]);
        if (candidates.size() == 1 || narrowest)
            result.format = candidates.front();
        return result;
    }

    if (best.isMuse16()) {
        result.type = InputProbeResult::Type::eMuse16Baseband;
        result.line_period = 480;
        result.line_strength = best.line_strength;
        result.sample_frequency = 16.2e6;
        result.sample_frequency_snapped = true;
        return result;
    }

    result.line_period = best.line_period;
    result.line_strength = best.line_strength;
    result.cycles_per_line = best.cycles_per_line;
    result.audio_carrier_ratio = best.audio_carrier_ratio;

    // Phase-correct 16.2 MHz MUSE baseband is exactly 480 samples per line and
    // has no FM carrier for the mean frequency to sit on.  Otherwise the audio
    // carrier is decisive when present -- the measured ratio is ~10^4 on real
    // NTSC RF and ~1.5 elsewhere -- and beyond that, classify on the carrier
    // frequency: ~520 cycles/line for NTSC, ~340 for MUSE, with the gap left
    // unclassified rather than guessed (content brightness moves the mean).
    using Type = InputProbeResult::Type;
    if (abs(result.line_period - 480.0) / 480.0 < 0.005 && result.cycles_per_line < 250) {
        result.type = Type::eMuse16Baseband;
        result.sample_frequency = 16.2e6;
        result.sample_frequency_snapped = true;
        return result;
    }
    if (result.audio_carrier_ratio > 50 || result.cycles_per_line >= 470)
        result.type = Type::eNtscRf;
    else if (result.cycles_per_line >= 260 && result.cycles_per_line <= 430)
        result.type = Type::eMuseRf;
    else
        return result;

    result.sample_frequency = estimateSampleFrequency(result, result.type);
    for (double rate : c_known_rates)
        if (result.sample_frequency == rate)
            result.sample_frequency_snapped = true;
    return result;
}

double estimateSampleFrequency(const InputProbeResult &result, InputProbeResult::Type type) {
    if (type == InputProbeResult::Type::eMuse16Baseband)
        return 16.2e6;
    if (result.line_period == 0 || type == InputProbeResult::Type::eUnknown)
        return 0;
    double fs = result.line_period *
            (type == InputProbeResult::Type::eNtscRf ? c_ntsc_line_hz : c_muse_line_hz);
    for (double rate : c_known_rates)
        if (abs(fs - rate) / rate < 0.012)
            return rate;
    return fs;
}
