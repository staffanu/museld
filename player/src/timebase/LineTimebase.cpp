// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include "LineTimebase.h"
#include "logging/Logger.h"

using namespace std;

namespace {
    // Kalman noise model, expressed in seconds so it is capture-rate
    // independent.  The sync timestamp noise is only the initial value: the
    // running estimate from the data takes over (floored), because the
    // innovation-based period-noise adaptation is only sound when R is
    // right -- with R asserted too large the normalized innovations read
    // small and the adaptation stiffens the filter until it can no longer
    // follow the wow.  The per-line period process noise is what the
    // adaptation scales; its floor keeps the filter able to follow the
    // measured period slew (up to ~2 ns per line on the white squall disc)
    // whatever the adaptation does.
    constexpr double c_sigma_meas_s = 42e-9;
    constexpr double c_sigma_meas_floor_s = 8e-9;
    constexpr double c_sigma_qp_s = 4.8e-9;
    constexpr double c_sigma_qt_s = 1.6e-9;
    constexpr double c_qscale_min = 0.5, c_qscale_max = 30.0;
    constexpr double c_kal_gate = 5.0; // innovation gate, in predicted sigmas
    // Two pulses corroborate each other when they lie a whole number of lines
    // apart to this tolerance per line of separation (wow moves a line by a
    // few tens of ns; junk sits microseconds off)
    constexpr double c_corroborate_s = 0.5e-6;
}

LineTimebase::LineTimebase(Logger &log, const char *owner, double sample_rate, double p_nominal, int lines_per_frame)
        : m_log(log),
          m_owner(owner),
          m_sample_rate(sample_rate),
          m_p_nominal(p_nominal),
          m_lines_per_frame(lines_per_frame),
          m_lattice_valid(false),
          m_lat_t(0),
          m_p_run(p_nominal),
          m_last_meas_k(0),
          m_slip_shift(0),
          m_slip_first_k(0),
          m_slip_count(0),
          m_last_dt(0),
          m_last_dk(0),
          m_d2_mean(0.7979 * sqrt(6.0) * c_sigma_meas_s * sample_rate),
          m_kal_valid(false),
          m_kal{},
          m_kal_head{},
          m_kal_head_k(0),
          m_k_final(0),
          m_qscale(1.0),
          m_nis_avg(1.0),
          m_adapted_to_k(-1),
          m_frame_resid_bad(0),
          m_frame_meas(0),
          m_frame_log_k(0),
          m_curve_base(0) {
}

void LineTimebase::reset() {
    m_lattice_valid = false;
    m_kal_valid = false;
    m_kal_head_k = 0;
    m_meas.clear();
    m_curve.clear();
    m_qscale = 1.0;
    m_nis_avg = 1.0;
    m_adapted_to_k = -1;
    m_k_final = 0;
    m_last_meas_k = 0;
    m_slip_count = 0;
    m_last_dt = 0;
    m_last_dk = 0;
    m_d2_mean = 0.7979 * sqrt(6.0) * c_sigma_meas_s * m_sample_rate;
    m_frame_resid_bad = 0;
    m_frame_meas = 0;
    m_frame_log_k = 0;
}

int64_t LineTimebase::addPulse(double t) {
    // Lattice assignment by chaining from the previous accepted pulse: the
    // number of lines between two pulses is their measured interval divided
    // by the period, rounded -- unambiguous for gaps of hundreds of lines,
    // since the period is known a priori to 0.1% and tracked here only from
    // clean consecutive intervals.  Crucially the period used for this never
    // comes from the filter.  Assigning line numbers from the filter's own
    // period estimate closes a positive-feedback loop: a small period error
    // pushes real pulses across the assignment boundary, they flip to the
    // next line with a large residual, the fit shortens the period further,
    // and the reconstruction runs away onto a self-consistent false lattice
    // (measured: a 13% short line period settling in a few seconds into the
    // white squall capture and never recovering).
    if (!m_lattice_valid) {
        m_lattice_valid = true;
        m_lat_t = t;
        m_p_run = m_p_nominal;
        m_meas.emplace_back(0, t);
        m_last_meas_k = 0;
        return -1;
    }
    const double dt = t - m_lat_t;
    const int64_t dk = llround(dt / m_p_run);
    if (dk < 1)
        return -1; // duplicate or false pulse within the same line
    const double err = abs(dt / m_p_run - (double)dk);
    int64_t k = m_last_meas_k + dk;

    // The curve is consulted only for INTEGER-line disagreement.  A junk
    // pulse can still insert a bogus line into the chain, after which every
    // real pulse sits exactly a line away from where the curve predicts it.
    // Fractional disagreement is never acted on (that is the feedback path
    // above); an integer shift confirmed by several consecutive pulses
    // re-labels the chain and the pulses stored since the slip.  Pulses
    // arrive up to a batch past the look-ahead head, where the extrapolation
    // is good to a small fraction of a line even at the wow extremes; a head
    // gone stale or uncertain after a long gap is not consulted.
    if (m_kal_valid && m_kal_head_k > 0 && k > m_kal_head_k && k - m_kal_head_k <= 2 * c_kal_batch
        && m_kal_head.P00 < pow(0.2 * m_kal_head.p, 2)) {
        const double resid = t - (m_kal_head.t + (double)(k - m_kal_head_k) * m_kal_head.p);
        const int64_t shift = llround(resid / m_kal_head.p);
        if (shift == 0) {
            m_slip_count = 0;
        } else if (m_slip_count == 0 || shift != m_slip_shift) {
            m_slip_shift = shift;
            m_slip_first_k = k;
            m_slip_count = 1;
        } else if (++m_slip_count >= 16) {
            m_log.warn(eInput, std::format("{}: lattice slipped {} line(s) at line {}, re-labeling",
                                           m_owner, shift, m_slip_first_k));
            for (auto &m : m_meas)
                if (m.first >= m_slip_first_k)
                    m.first += shift;
            m_last_meas_k += shift;
            k += shift;
            m_slip_count = 0;
        }
    }

    if (k <= m_last_meas_k)
        return -1; // duplicate line
    m_lat_t = t; // last accepted pulse: chain reference and no-sync watchdog
    m_meas.emplace_back(k, t);
    m_last_meas_k = k;
    if (dk == 1 && err < 0.1)
        m_p_run += 0.02 * (dt - m_p_run); // the local period, from clean consecutive intervals only
    // Measurement noise from the data: the second difference of three
    // consecutive-line pulse times has variance 6R plus a negligible wow
    // term.  Mean absolute value, clipped for robustness against junk.
    if (dk == 1 && m_last_dk == 1) {
        const double d2 = abs(dt - m_last_dt);
        m_d2_mean += 0.002 * (min(d2, 5.0 * m_d2_mean) - m_d2_mean);
    }
    m_last_dt = dt;
    m_last_dk = dk;

    if (!m_kal_valid) {
        m_kal_valid = true;
        m_kal.t = m_meas.front().second - m_p_nominal;
        m_kal.p = m_p_nominal;
        m_kal.P00 = 4.0 * m_p_nominal * m_p_nominal;
        m_kal.P01 = 0.0;
        m_kal.P11 = pow(2e-3 * m_p_nominal, 2);
        m_k_final = m_meas.front().first;
        m_curve_base = m_k_final;
    }
    return k;
}

void LineTimebase::setPeriod(double p) {
    m_p_run = p;
    if (m_kal_valid)
        m_kal.p = p;
}

double LineTimebase::predict(int64_t k) const {
    if (m_kal_valid && m_kal_head_k > 0 && k > m_kal_head_k && k - m_kal_head_k <= 2 * c_kal_batch
        && m_kal_head.P00 < pow(0.2 * m_kal_head.p, 2))
        return m_kal_head.t + (double)(k - m_kal_head_k) * m_kal_head.p;
    return m_lat_t + (double)(k - m_last_meas_k) * m_p_run;
}

bool LineTimebase::canFinalize() const {
    return m_kal_valid && m_last_meas_k >= m_k_final + c_kal_batch + c_kal_lag;
}

void LineTimebase::finalizeBatch() {
    constexpr int W = c_kal_batch + c_kal_lag;
    double meas[W];
    bool has_meas[W] = {};
    for (int i = 0; i < W; i++)
        meas[i] = 0;
    for (auto &[k, t] : m_meas) {
        const int64_t i = k - m_k_final;
        if (i >= 0 && i < W) {
            meas[i] = t;
            has_meas[i] = true;
        }
    }

    // E|d2| = 0.798 sigma_d2 for Gaussian noise, and var(d2) = 6R
    const double sigma_d2 = 1.2533 * m_d2_mean;
    const double R = max(sigma_d2 * sigma_d2 / 6.0, pow(c_sigma_meas_floor_s * m_sample_rate, 2));
    const double q00 = pow(c_sigma_qt_s * m_sample_rate, 2);

    KalState pred[W], filt[W];
    KalState st = m_kal;
    for (int i = 0; i < W; i++) {
        const double qpp = pow(c_sigma_qp_s * m_sample_rate * m_qscale, 2);
        st.t += st.p;
        st.P00 += 2 * st.P01 + st.P11 + q00;
        st.P01 += st.P11;
        st.P11 += qpp;
        pred[i] = st;
        if (has_meas[i]) {
            const double y = meas[i] - st.t;
            const double S = st.P00 + R;
            const double gate2 = c_kal_gate * c_kal_gate * S;
            // A measurement beyond the gate is junk when it stands alone, and
            // the filter's own error when the following pulses agree with it.
            // A hard gate cannot tell the two apart and deadlocks on the
            // second: once the filter has fallen behind (a period slew it was
            // too stiff for, a time-base step), every real pulse is rejected,
            // and the predicted variance of a rejected filter grows far too
            // slowly for the gate to catch up with the runaway innovation --
            // the curve then coasts for the rest of the file.  So an outlier
            // is rejected only when the next measurement fails to corroborate
            // it (a whole number of lines away, by the chain's period).  A
            // corroborated one is absorbed with bounded influence -- the
            // Huber weight inflates its noise so it counts as if it sat at
            // the gate -- which pulls a lost filter back onto the data within
            // tens of lines while two spurious pulses a line apart can only
            // nudge it.
            double R_eff = R;
            bool accept = true;
            if (y * y > gate2) {
                accept = false;
                for (int j = i + 1; j < W && j <= i + 3; j++)
                    if (has_meas[j]) {
                        accept = abs(meas[j] - meas[i] - (double)(j - i) * m_p_run)
                                 < (double)(j - i) * c_corroborate_s * m_sample_rate;
                        break;
                    }
                R_eff = R * sqrt(y * y / gate2);
            }
            if (accept) {
                const double S_eff = st.P00 + R_eff;
                const double K0 = st.P00 / S_eff, K1 = st.P01 / S_eff;
                st.t += K0 * y;
                st.p += K1 * y;
                const double P01_pre = st.P01;
                st.P00 *= (1 - K0);
                st.P01 *= (1 - K0);
                st.P11 -= K1 * P01_pre;
                // Innovation-based adaptation of the period process noise: a
                // filter does not estimate its own noise model, and the wow
                // amplitude varies over the disc (radius, player servo), so
                // the normalized innovation variance steers a slow scale
                if (m_k_final + i > m_adapted_to_k) {
                    m_nis_avg += 0.005 * (min(y * y / S, c_kal_gate * c_kal_gate) - m_nis_avg);
                    if (m_nis_avg > 1.3)
                        m_qscale = min(m_qscale * 1.003, c_qscale_max);
                    else if (m_nis_avg < 0.7)
                        m_qscale = max(m_qscale * 0.998, c_qscale_min);
                }
            }
        }
        filt[i] = st;
    }
    m_adapted_to_k = m_k_final + W - 1;

    // RTS backward pass over the window; only the first c_kal_batch lines are
    // final, the rest is the look-ahead that will be re-run next batch
    KalState sm[W];
    sm[W - 1] = filt[W - 1];
    for (int i = W - 2; i >= 0; i--) {
        const KalState &f = filt[i];
        const KalState &pr = pred[i + 1];
        const double det = pr.P00 * pr.P11 - pr.P01 * pr.P01;
        // A = P_filt * F^T for F = [[1,1],[0,1]]
        const double A00 = f.P00 + f.P01, A01 = f.P01;
        const double A10 = f.P01 + f.P11, A11 = f.P11;
        const double G00 = (A00 * pr.P11 - A01 * pr.P01) / det;
        const double G01 = (-A00 * pr.P01 + A01 * pr.P00) / det;
        const double G10 = (A10 * pr.P11 - A11 * pr.P01) / det;
        const double G11 = (-A10 * pr.P01 + A11 * pr.P00) / det;
        const double dxt = sm[i + 1].t - pr.t, dxp = sm[i + 1].p - pr.p;
        sm[i] = f;
        sm[i].t += G00 * dxt + G01 * dxp;
        sm[i].p += G10 * dxt + G11 * dxp;
    }

    // Debug aid: MUSELD_DUMP_TIMEBASE=<prefix> appends the accepted (k, t)
    // measurements and the finalized curve for offline comparison
    static const char *dump_prefix = getenv("MUSELD_DUMP_TIMEBASE");
    if (dump_prefix != nullptr) {
        if (FILE *f = fopen((std::string(dump_prefix) + ".curve.f64").c_str(), "ab")) {
            for (int i = 0; i < c_kal_batch; i++) {
                double rec[2] = {(double)(m_k_final + i), sm[i].t};
                fwrite(rec, sizeof rec, 1, f);
            }
            fclose(f);
        }
        if (FILE *f = fopen((std::string(dump_prefix) + ".meas.f64").c_str(), "ab")) {
            for (auto &[k, t] : m_meas)
                if (k >= m_k_final && k < m_k_final + c_kal_batch) {
                    double rec[2] = {(double)k, t};
                    fwrite(rec, sizeof rec, 1, f);
                }
            fclose(f);
        }
    }

    // Per-frame sync quality: over the lines being finalized, count how many
    // carried a sync and how many of those disagree with the smoothed curve
    // by more than 2 us.  This reports on the timebase without ever looking
    // at the decoded picture -- a raw-vs-reconstructed check, computed
    // against the final curve rather than the forward filter's coasting
    // prediction.  (No step detector lives here any more: analysis of the
    // raw pulse stream showed the disturbances on even the worst disc are
    // smooth and continuous -- wow and crosstalk bias, nothing discontinuous
    // -- and a genuine time-base step recovers by itself, the rejections
    // growing the predicted variance until the widened gate absorbs it.)
    for (int i = 0; i < c_kal_batch; i++)
        m_curve.push_back(sm[i].t);
    // A finalized line is "supported" when a detected pulse falls within 2 us
    // of its curve position; the unsupported ones are where sync is genuinely
    // missing or corrupt and the picture can weave.  (Counting instead every
    // measurement that disagrees with the curve would be backwards: the junk
    // and half-line pulses far from the curve are exactly what the gate is
    // meant to reject, and rejecting them is the curve working, not failing.)
    bool supported[c_kal_batch] = {};
    for (auto &[k, t] : m_meas)
        if (k >= m_k_final && k < m_k_final + c_kal_batch
            && abs(t - sm[k - m_k_final].t) < 2.0e-6 * m_sample_rate)
            supported[k - m_k_final] = true;
    for (int i = 0; i < c_kal_batch; i++) {
        m_frame_meas++;
        if (!supported[i])
            m_frame_resid_bad++;
    }
    m_kal_head = filt[W - 1];
    m_kal_head_k = m_k_final + W - 1;
    m_kal = filt[c_kal_batch - 1];
    m_k_final += c_kal_batch;

    while (m_k_final - m_frame_log_k >= m_lines_per_frame) {
        m_frame_log_k += m_lines_per_frame;
        m_log.debug(eInput, std::format("timebase to line {}: {} of {} lines unsupported by a nearby sync pulse; "
                                        "sigma_meas {:.1f} ns, qscale {:.2f}",
                                        m_frame_log_k, m_frame_resid_bad, m_frame_meas,
                                        sqrt(R) / m_sample_rate * 1e9, m_qscale));
        m_frame_resid_bad = 0;
        m_frame_meas = 0;
    }

    while (!m_meas.empty() && m_meas.front().first < m_k_final)
        m_meas.pop_front();
}

bool LineTimebase::locate(double t, int64_t *k_out, double *phase_out) const {
    if (m_curve.size() < 2)
        return false;
    size_t lo = 0, hi = m_curve.size() - 1;
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        (m_curve[mid] <= t ? lo : hi) = mid;
    }
    if (m_curve[lo] > t || m_curve[lo + 1] <= t)
        return false;
    *k_out = m_curve_base + (int64_t)lo;
    *phase_out = (t - m_curve[lo]) / (m_curve[lo + 1] - m_curve[lo]);
    return true;
}
