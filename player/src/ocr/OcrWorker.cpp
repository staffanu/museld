// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "OcrWorker.h"

#include <algorithm>
#include <chrono>
#include <format>

#include "OcrEngine.h"

namespace {

// Ported from tools/subocr/subocr.py: subtitle-ish pixels are bright,
// low-saturation, and have a dark (outline) pixel nearby.
constexpr int c_mask_bright = 190;
constexpr int c_mask_max_saturation = 40;
constexpr int c_mask_dark = 70;
constexpr int c_mask_neighborhood = 5;
constexpr int c_mask_min_pixels = 200;    // below this the band counts as empty
constexpr float c_mask_diff_thresh = 0.15f; // relative change that triggers OCR

std::vector<uint8_t> textMask(const uint8_t *rgb, int width, int height) {
    std::vector<uint8_t> bright((size_t)width * height), dark((size_t)width * height);
    for (size_t p = 0; p < (size_t)width * height; p++) {
        int r = rgb[p * 3], g = rgb[p * 3 + 1], b = rgb[p * 3 + 2];
        int lum = (r + g + b) / 3;
        int sat = std::max({r, g, b}) - std::min({r, g, b});
        bright[p] = lum > c_mask_bright && sat < c_mask_max_saturation;
        dark[p] = lum < c_mask_dark;
    }
    // bright pixel with a dark pixel within the neighborhood (checked at
    // offsets 0/±n on each axis, like the python prototype's shifted ORs)
    std::vector<uint8_t> mask((size_t)width * height, 0);
    constexpr int n = c_mask_neighborhood;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (!bright[(size_t)y * width + x]) continue;
            for (int dy = -n; dy <= n && !mask[(size_t)y * width + x]; dy += n)
                for (int dx = -n; dx <= n; dx += n) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    if (dark[(size_t)ny * width + nx]) {
                        mask[(size_t)y * width + x] = 1;
                        break;
                    }
                }
        }
    }
    return mask;
}

bool maskChanged(const std::vector<uint8_t> &prev, const std::vector<uint8_t> &cur) {
    if (prev.size() != cur.size()) return true;
    size_t a = 0, b = 0, differ = 0;
    for (size_t i = 0; i < cur.size(); i++) {
        a += prev[i];
        b += cur[i];
        differ += prev[i] != cur[i];
    }
    if (std::max(a, b) < c_mask_min_pixels) return (a >= c_mask_min_pixels) != (b >= c_mask_min_pixels);
    return (float)differ / (float)std::max(a, b) > c_mask_diff_thresh;
}

} // namespace

OcrWorker::OcrWorker(Logger &log, const std::string &det_model_path,
                     const std::string &rec_model_path)
        : m_log(log),
          m_thread(&OcrWorker::threadFunc, this, det_model_path, rec_model_path) {
}

OcrWorker::~OcrWorker() {
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void OcrWorker::submitBand(std::vector<uint8_t> rgb, int width, int height, double stream_seconds) {
    {
        std::scoped_lock lock(m_mutex);
        if (!m_pending_rgb.empty()) m_dropped_samples++;
        m_pending_rgb = std::move(rgb);
        m_pending_width = width;
        m_pending_height = height;
        m_pending_seconds = stream_seconds;
    }
    m_cv.notify_all();
}

std::vector<OcrWorker::Update> OcrWorker::drainUpdates() {
    std::scoped_lock lock(m_mutex);
    return std::exchange(m_updates, {});
}

void OcrWorker::threadFunc(std::string det_model_path, std::string rec_model_path) {
    std::unique_ptr<OcrEngine> engine;
    try {
        engine = std::make_unique<OcrEngine>(m_log, det_model_path, rec_model_path);
    } catch (const std::exception &x) {
        m_log.error(eApplication, std::format("OCR disabled: {}", x.what()));
        return;
    }

    std::vector<uint8_t> prev_mask;
    std::vector<std::string> last_lines;
    // After a text change, re-OCR the next sample even if the mask is stable:
    // a sample can catch the subtitle mid-fade, and the partial reading would
    // otherwise stick for the whole cue (the mask barely changes as the last
    // fade step completes)
    bool confirm_next = false;
    for (;;) {
        std::vector<uint8_t> rgb;
        int width, height;
        double seconds;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_pending_rgb.empty(); });
            if (m_stop) break;
            rgb = std::move(m_pending_rgb);
            m_pending_rgb.clear();
            width = m_pending_width;
            height = m_pending_height;
            seconds = m_pending_seconds;
        }

        auto mask = textMask(rgb.data(), width, height);
        const bool changed = maskChanged(prev_mask, mask);
        prev_mask = std::move(mask);
        if (!changed && !confirm_next) continue;

        size_t mask_pixels = 0;
        for (uint8_t m : prev_mask) mask_pixels += m;
        std::vector<std::string> lines;
        if (mask_pixels >= c_mask_min_pixels) {
            const auto t0 = std::chrono::steady_clock::now();
            lines = OcrEngine::assembleLines(engine->recognize(rgb.data(), width, height), true);
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            long dropped;
            {
                std::scoped_lock lock(m_mutex);
                dropped = std::exchange(m_dropped_samples, 0L);
            }
            // An OCR slower than the sampling cadence drops band samples --
            // fine occasionally, but sustained lag can skip short cues
            m_log.log(elapsed_ms > 1000 ? eWarn : eDebug, eApplication,
                      std::format("OCR inference took {} ms ({} band samples dropped meanwhile)",
                                  elapsed_ms, dropped));
        }

        if (lines != last_lines) {
            last_lines = lines;
            confirm_next = true;
            std::scoped_lock lock(m_mutex);
            m_updates.push_back({seconds, std::move(lines)});
        } else {
            confirm_next = false;
        }
    }
}
