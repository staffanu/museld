// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_LINETIMEBASE_H
#define MUSELD_LINETIMEBASE_H

#include <cstdint>
#include <deque>
#include <utility>

class Logger;

// The feed-forward line timebase that the SDTV and MUSE readers share: sync
// measurements (one stream position per line, however the standard's sync is
// found) are assigned to an integer line lattice by chaining measured
// intervals, a fixed-lag Kalman RTS smoother fits the curve T(k) -- the
// stream position of line k's sync -- through them, and the finalized part
// of the curve is handed out line by line for resampling.  What the sync
// looks like, where the frame starts on the lattice (the vertical anchor)
// and the resampling itself are the readers' business.
//
// Three things had to be right for this to work, each found with an
// independent measurement rather than by watching the picture (see the
// NTSC commit that introduced it):
//
// - Line numbers are never assigned from the filter's own period.  Doing so
//   closes a positive-feedback loop (a small period error pushes real pulses
//   across the assignment boundary, the fit shortens the period further)
//   that settled the reconstruction onto a 13% short lattice a few seconds
//   in.  The chain's period learns only from intervals already known to be
//   single lines; the curve is consulted only for integer-line slips.
// - A hard innovation gate deadlocks: once the filter lags, every real pulse
//   is rejected and the predicted variance grows far too slowly for the gate
//   to catch up.  Measurement noise is estimated from second differences of
//   consecutive pulse times, the Q scale is floored, and an outlier is
//   rejected only when the next pulse fails to corroborate it; a
//   corroborated one gets a bounded-influence (Huber) update.
// - The measurement itself must not depend on the picture (the readers'
//   concern: the NTSC slicing level comes from the back porch, the MUSE edge
//   from its own plateaus).
class LineTimebase {
public:
    // owner names the reader in the log; sample_rate is the stream's (the
    // demodulated, video-decimated rate), p_nominal the nominal line period
    // in its samples, lines_per_frame sets the cadence of the quality log
    LineTimebase(Logger &log, const char *owner, double sample_rate, double p_nominal, int lines_per_frame);

    void reset();

    // A sync measurement at stream position t.  Returns the lattice line it
    // was chained to, or -1 when it was dropped (a second pulse on the same
    // line, or the first pulse of a new lattice, which only seeds it).
    int64_t addPulse(double t);

    [[nodiscard]] bool latticeValid() const { return m_lattice_valid; }
    [[nodiscard]] double lastPulseTime() const { return m_lat_t; }
    [[nodiscard]] int64_t lastPulseLine() const { return m_last_meas_k; }
    [[nodiscard]] double period() const { return m_p_run; } // the chain's local line period
    // Seed the period from a measurement the chain cannot make quickly (the
    // MUSE acquisition chain's, good to 0.05% over its 32 lines): the chain
    // adapts at 2% per line and the filter learns it slower still, and a
    // player spinning up is 1-2% off nominal
    void setPeriod(double p);
    // Where line k is expected: from the look-ahead head of the filter when
    // it has one near enough, else by the chain's period from the last pulse
    [[nodiscard]] double predict(int64_t k) const;

    [[nodiscard]] bool canFinalize() const;
    void finalizeBatch();

    // The finalized curve: T(curveBase() + i) for i < curveSize()
    [[nodiscard]] size_t curveSize() const { return m_curve.size(); }
    [[nodiscard]] double curve(size_t i) const { return m_curve[i]; }
    [[nodiscard]] int64_t curveBase() const { return m_curve_base; }
    void popLine() { m_curve.pop_front(); m_curve_base++; }
    // The lattice line whose curve interval holds t, and where in it (0..1)
    [[nodiscard]] bool locate(double t, int64_t *k_out, double *phase_out) const;

    static constexpr int c_kal_batch = 64;  // lines finalized per batch
    static constexpr int c_kal_lag = 128;   // look-ahead beyond the batch

private:
    struct KalState {
        double t, p;                    // position of line start, period
        double P00, P01, P11;           // symmetric covariance
    };

    Logger &m_log;
    const char *m_owner;
    const double m_sample_rate;
    const double m_p_nominal;
    const int m_lines_per_frame;

    // --- lattice: integer line numbers for sync timestamps ---
    // Chained pulse to pulse: the nominal period is known a priori to ~0.1%
    // (wow included), so round(dt / period) is unambiguous for gaps of
    // hundreds of lines and needs no lock-in phase.  The period is tracked
    // from clean consecutive intervals only, never from the filter (see
    // addPulse for the runaway that causes).
    bool m_lattice_valid;
    double m_lat_t;                     // last accepted pulse
    double m_p_run;                     // slowly adapted local period
    int64_t m_last_meas_k;              // its line
    // Integer-line slip correction against the curve: a shift confirmed by
    // this many consecutive pulses re-labels the chain
    int64_t m_slip_shift;
    int64_t m_slip_first_k;
    int m_slip_count;
    // Running measurement-noise estimate: mean |second difference| of
    // consecutive-line pulse times (see addPulse)
    double m_last_dt;
    int64_t m_last_dk;
    double m_d2_mean;
    std::deque<std::pair<int64_t, double>> m_meas; // (k, t) measurements

    // --- fixed-lag Kalman smoother over [position, period] ---
    bool m_kal_valid;
    KalState m_kal;                     // filtered state after line m_k_final - 1
    KalState m_kal_head;                // newest filtered state (look-ahead head)
    int64_t m_kal_head_k;               // its line, 0 while unset
    int64_t m_k_final;                  // first line not yet finalized
    // Innovation-based adaptation of the period process noise: the filter
    // does not estimate its own noise model, so the normalized innovation
    // variance steers a slow scale factor -- wow varies with disc radius
    double m_qscale;
    double m_nis_avg;
    int64_t m_adapted_to_k;
    // Per-frame sync quality (logged at debug level): how many lines had a
    // sync, and how many of those disagreed with the fitted curve
    int m_frame_resid_bad;
    int m_frame_meas;
    int64_t m_frame_log_k;

    std::deque<double> m_curve;         // finalized T(k), k from m_curve_base
    int64_t m_curve_base;
};

#endif //MUSELD_LINETIMEBASE_H
