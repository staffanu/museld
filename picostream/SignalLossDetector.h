#ifndef PICOSTREAM_SIGNALLOSSDETECTOR_H
#define PICOSTREAM_SIGNALLOSSDETECTOR_H

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <vector>

// Detects that the analog input has gone silent (e.g. the laserdisc player stopped at the end
// of the disc), so a capture can stop instead of recording noise forever.  Adapted from the
// SignalLossDetector in the fx3usbadc/usbstream utility, without its AGC gain normalization
// since the Picoscope gain (voltage range) is fixed for the whole capture.
class SignalLossDetector {
public:
  SignalLossDetector(double timeout_seconds, double threshold, bool is8bit)
    : m_timeout_seconds(timeout_seconds), m_threshold(threshold), m_is8bit(is8bit) {
  }

  // length is number of samples; returns true once the signal has stayed below the threshold
  // for longer than the configured timeout
  bool process(const void *data, size_t length) {
    int samples_to_process = std::min((int)length, c_samples_to_inspect);
    if (samples_to_process <= 0)
      return false;

    m_magnitudes.clear();
    for (int i = 0; i < samples_to_process; i++) {
      int value = m_is8bit ? ((const int8_t *)data)[i] : ((const int16_t *)data)[i];
      m_magnitudes.push_back(std::abs(value));
    }
    // Measure the level as a high quantile rather than the max, so that impulsive noise in an
    // otherwise dead signal does not read as signal.
    int quantile_index = (int)((samples_to_process - 1) * c_quantile);
    std::nth_element(m_magnitudes.begin(), m_magnitudes.begin() + quantile_index, m_magnitudes.end());
    double level = m_magnitudes[quantile_index];

    auto now = std::chrono::steady_clock::now();
    if (!m_initialized) {
      m_initialized = true;
      m_average_level = level;
      m_last_update = now;
      return false;
    }
    double dt = std::chrono::duration<double>(now - m_last_update).count();
    m_last_update = now;

    if (level < m_threshold * m_average_level) {
      if (!m_in_signal_loss) {
        m_in_signal_loss = true;
        m_signal_loss_start = now;
      }
      return std::chrono::duration<double>(now - m_signal_loss_start).count() >= m_timeout_seconds;
    }
    m_in_signal_loss = false;

    // The average follows the signal level slowly.  Frames classified as signal loss are
    // excluded (above), so a long stretch of silence cannot drag the average down towards the
    // noise floor and make the detection level meaningless.
    double alpha = std::min(1.0, dt / c_average_time_constant_seconds);
    m_average_level += alpha * (level - m_average_level);
    return false;
  }

private:
  static const int c_samples_to_inspect = 16384;
  static constexpr double c_quantile = 0.95;
  static constexpr double c_average_time_constant_seconds = 30.0;

  double m_timeout_seconds;
  double m_threshold;
  bool m_is8bit;

  bool m_initialized{};
  double m_average_level{};
  bool m_in_signal_loss{};
  std::chrono::steady_clock::time_point m_signal_loss_start;
  std::chrono::steady_clock::time_point m_last_update;
  std::vector<int> m_magnitudes; // reused between blocks to avoid reallocation
};

#endif // PICOSTREAM_SIGNALLOSSDETECTOR_H
