// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cassert>
#include <cmath>
#include <format>
#include "SlowArErasureConcealer.h"
#include "../../filter/Window.h"

/*
* This class implements the algorithm presented in "Interpolation of Missing Samples in Audio Signals
* Based on Autoregressive Modeling" by Laurent Oudre, and is heavily based on his published implementation,
* which is licensed under the GNU General Public License v3 or later.  The changes are to the main
* outer loop, which takes input data in chunks instead of processing an entire file, and also, code
* was reformatted and uses some C++ features.
*/

SlowArErasureConcealer::SlowArErasureConcealer(Logger &log, int order, int hop_size, std::optional<std::string> debug_filename) :
    ErasureConcealer(log, debug_filename),
    m_log(log),
    m_order(order),
    m_hop_size(hop_size),
    m_window_size(hop_size * 4)
{
    for (int i = 0; i < 3; i++)
        for (int ch = 0; ch < 2; ch++)
            m_output_buffers[ch].push_back(std::vector<double>(m_window_size));

    assert(m_window_size % 4 == 0);
    m_hamming_window = Window::hamming(m_window_size + 1);
    m_hamming_window.resize(m_window_size);
    // Check that the window is COLA (constant overlap and add) and compute the constant gain
    double ola_gain;
    for (int i = 0; i < m_window_size / 4; i++) {
        double sum = 0;
        for (int j = 0; j < 4; j++)
            sum += m_hamming_window[i + j * m_window_size / 4];
        if (i == 0)
            ola_gain = sum;
        else
            assert(std::abs(sum - ola_gain) < 1e-10);
    }

    // Make the gain 1.0
    for (int i = 0; i < m_window_size; i++)
        m_hamming_window[i] *= 1 / ola_gain;
}

std::vector<TwoChannelSampleWithErasureFlags> SlowArErasureConcealer::processSamplesImpl(
    const std::vector<TwoChannelSampleWithErasureFlags> &samples_with_erasures) {

    m_samples.insert(m_samples.end(), samples_with_erasures.begin(), samples_with_erasures.end());
    std::vector<TwoChannelSampleWithErasureFlags> output;
    while (m_samples.size() >= m_window_size) {
        assert(m_output_buffers[0].size() == 3);
        assert(m_output_buffers[1].size() == 3);

        std::vector<double> left;
        std::vector<double> right;
        processFrame(std::span(m_samples).subspan(0, m_window_size), left, right);
        m_output_buffers[0].push_back(left);
        m_output_buffers[1].push_back(right);

        for (int i = 0; i < m_hop_size; i++) {
            TwoChannelSampleWithErasureFlags sample{};
            for (int ch = 0; ch < 2; ch++) {
                double sum = 0.0;
                for (int j = 0; j < 4; j++) {
                    sum += m_output_buffers[ch][j][i + (3 - j) * m_hop_size];
                }
                sample.samples[ch] = (int16_t)round(sum);
                sample.erased[ch] = false; // TODO: check if concealment worked!
            }
            output.push_back(sample);
        }

        m_output_buffers[0].pop_front();
        m_output_buffers[1].pop_front();

        m_samples.erase(m_samples.begin(), m_samples.begin() + m_hop_size);
    }
    return output;
}

void SlowArErasureConcealer::processFrame(std::span<TwoChannelSampleWithErasureFlags> samples, std::vector<double> &left_out, std::vector<double> &right_out) const {
    std::vector<double> left_in(m_window_size);
    std::vector<double> right_in(m_window_size);
    std::vector<bool> left_erased_in(m_window_size);
    std::vector<bool> right_erased_in(m_window_size);

    assert(samples.size() == m_window_size);
    for (int i = 0; i < m_window_size; i++) {
        left_in[i] = samples[i].samples[0];
        right_in[i] = samples[i].samples[1];
        left_erased_in[i] = samples[i].erased[0];
        right_erased_in[i] = samples[i].erased[1];
        if (left_erased_in[i])
            left_in[i] = 0.0;
        if (right_erased_in[i])
            right_in[i] = 0.0;
    }
    reconstruct_signal(left_in, left_erased_in, left_out);
    reconstruct_signal(right_in, right_erased_in, right_out);
}

double SlowArErasureConcealer::dotProduct(const std::vector<double> &x, int i_minx, int i_maxx, const std::vector<double> &y, int i_miny, int i_maxy) {
    const int Nx = x.size();
    const int Ny = y.size();
    assert(!(i_minx < 0 || i_maxx > Nx - 1 || i_miny < 0 || i_maxy > Ny - 1));
    const int N = i_maxx - i_minx + 1;
    assert(i_maxy - i_miny + 1 == N);

    double value = 0;
    for (int i = 0; i < N; i++)
        value += x[i + i_minx] * y[i + i_miny];

    return value;
}

bool SlowArErasureConcealer::isPresent(int value, const std::vector<int> &x) {
    if (value < x.front() || value > x.back())
        return false;

    int i_start = 0;
    int i_end = x.size() - 1;
    while (i_start <= i_end) {
        int i_current = (i_end + i_start) / 2;
        if (x[i_current] == value)
            return true;

        if (value < x[i_current])
            i_end = i_current-1;

        if (value > x[i_current])
            i_start = i_current+1;
    }
    return false;
}

void SlowArErasureConcealer::reconstruct_signal(std::vector<double> const &samples, std::vector<bool> const &burst, std::vector<double> &output) const {
    output.resize(m_window_size);

    int m = 0; // Number of missing samples
    for (int i = m_order; i < m_window_size - m_order; i++) {
        if (burst[i])
            m++;
        output[i] = samples[i];
    }

    if (m > 0) {
        std::vector<double> a(m_order + 1);
        std::vector<double> x(m); // Indexes of the missing samples
        std::vector<int> t(m); // Interpolated samples

        // Determination of the indexes of the missing samples
        // (The first and last p samples are not considered)
        int k = 0;
        for (int j = m_order; j < m_window_size - m_order; j++)
            if (burst[j])
                t[k++] = j;

        update_a(output, a);
        if (update_x(output, a, t, x))
            for (int j = 0; j < m; j++)
                output[t[j]] = x[j];
        else
            m_log.info(eAudio,
                std::format("Erasure concealment failed: singular matrix.  Windows size={}, erased values={}",
                    m_window_size, m));
    }

    for (int j = 0; j < m_window_size; j++)
        output[j] *= m_hamming_window[j];
}

void SlowArErasureConcealer::update_a(const std::vector<double> &y, std::vector<double> &a) const {
    double R[m_order+1];
    for (int i = 0; i < m_order + 1; i++)
        R[i] = dotProduct(y, i,m_window_size - 1, y, 0, m_window_size - i - 1) / m_window_size;

    // Levinson algorithm

    // Initialization
    double a_old[m_order];
    a_old[0] = -R[1] / R[0];
    double sigma2 = (1 - a_old[0] * a_old[0]) * R[0];
    a[0] = a_old[0];

    for (int j = 1; j < m_order; j++) {
        // Calculation of the reflection coefficient
        double k = 0;
        for (int i = 0; i < j; i++)
            k += a_old[i] * R[j - i];
        k = (R[j + 1] + k) / sigma2;

        // Update
        a[j] = -k;
        sigma2 = (1 - a[j] * a[j]) * sigma2;
        for (int i = j - 1; i >= 0; i--)
            a[i] = a_old[i] + a[j] * a_old[j - i - 1];

        for (int i = 0; i < j + 1; i++)
            a_old[i] = a[i];
    }

    a[0] = 1;
    for (int i = 0; i < m_order; i++)
        a[i + 1] = a_old[i];
}

bool SlowArErasureConcealer::update_x(const std::vector<double> &y, const std::vector<double> &a, const std::vector<int> &t,
                                  std::vector<double> &x) const {
    const int m = t.size();
    assert(x.size() == m);

    std::vector<std::vector<double>> B(m);
    for (int i = 0; i < m; i++)
        B[i].resize(m);

    std::vector<double> b(m_order+1);
    for (int i = 0; i < m_order + 1; i++)
        b[i] = dotProduct(a, 0, m_order - i, a, i, m_order);

    for (int i = 0; i < m; i++)
        for (int j = i; j < m; j++)
            if (abs(t[i] - t[j]) < m_order + 1) {
                B[i][j] = b[abs(t[i] - t[j])];
                B[j][i] = B[i][j];
            }

    for (int i = 0; i < m; i++) {
        x[i] = 0;
        for (int j = -m_order; j < m_order + 1; j++)
            if (!isPresent(t[i] - j, t))
                x[i] += b[abs(j)] * y[t[i] - j];
    }

    for (int i = 0; i < m; i++)
        x[i] = -x[i];

    return solveCholesky(B, x);
}

bool SlowArErasureConcealer::solveCholesky(const std::vector<std::vector<double> > &A, std::vector<double> &x) {
    const int N = x.size();
    assert(A.size() == N);
    // STEP 1 : Decomposition of A as L'DL where:
    //  L is a lower triangular matrix
    //  D is a diagonal matrix

    std::vector<std::vector<double>> L(N);
    for (int i = 0; i < N; i++)
        L[i].resize(N);

    std::vector<double> d(N);
    std::vector<double> v(N);

    for (int j = 0; j < N; j++) {
        if (j > 0) {
            for (int i = 0; i < j; i++)
                v[i] = L[j][i] * d[i];
            v[j] = A[j][j] - dotProduct(L[j], 0, j - 1, v, 0, j - 1);
        }
        else
            v[j] = A[j][j];
        d[j] = v[j];

        if (v[j] == 0)
            return false;

        if (j < N-1)
            for (int i = j + 1; i < N; i++)
                L[i][j] = (A[i][j] - dotProduct(L[i], 0, j-1, v, 0, j-1)) / v[j];
        L[j][j] = 1;
    }

    for (int i = 0; i < N; i++)
        for (int j = 0; j < i + 1; j++)
            L[j][i] = L[i][j];

    // STEP 2 : Solving two triangular systems
    for (int i = 0; i < N; i++)
        v[i] = 0;

    for (int i = 0; i < N; i++)
        v[i] = x[i] - dotProduct(L[i], 0, i, v, 0, i);

    for (int i = 0; i < N; i++)
        x[i] = 0;

    for (int i = N - 1; i >= 0; i--)
        x[i] = v[i] / d[i] - dotProduct(L[i], i, N-1, x, i, N-1);

    return true;
}
