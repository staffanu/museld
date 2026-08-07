// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_OCR_WORKER_H
#define MUSELD_OCR_WORKER_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logging/Logger.h"
#include "OcrEngine.h"

// Background OCR of the subtitle band ("museld-ocr" thread).  The render
// thread submits sampled band images (latest wins -- OCR slower than the
// sampling cadence just drops samples), and drains text updates between
// frames.  A cheap text-presence mask gates the actual OCR: it only runs when
// the band's subtitle-ish pixels change, so the model cost is per cue, not
// per sample.  Model loading happens on the worker thread, keeping startup
// off the render thread.
class OcrWorker {
public:
    // An update means: from stream time `seconds`, the subtitle band shows
    // `lines` (empty = no subtitle).  Consecutive updates always differ.
    struct Update {
        double seconds;
        std::vector<std::string> lines;
    };

    OcrWorker(Logger &log, const std::string &det_model_path, const std::string &rec_model_path,
              OcrScriptFilter script_filter);
    ~OcrWorker(); // joins the thread

    OcrWorker(const OcrWorker &) = delete;
    OcrWorker &operator=(const OcrWorker &) = delete;

    // Render thread: submit one sampled band (packed RGB8, width*height*3).
    void submitBand(std::vector<uint8_t> rgb, int width, int height, double stream_seconds);

    // Render thread: fetch the updates produced since the last call.
    std::vector<Update> drainUpdates();

private:
    void threadFunc(std::string det_model_path, std::string rec_model_path);

    Logger &m_log;
    const OcrScriptFilter m_script_filter;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    // Latest submitted band, consumed by the worker (empty rgb = none pending)
    std::vector<uint8_t> m_pending_rgb;
    int m_pending_width = 0, m_pending_height = 0;
    double m_pending_seconds = 0.0;
    long m_dropped_samples = 0; // bands overwritten while OCR was busy
    std::vector<Update> m_updates;

    std::thread m_thread;
};

#endif // MUSELD_OCR_WORKER_H
