// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_OCR_ENGINE_H
#define MUSELD_OCR_ENGINE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "logging/Logger.h"

namespace Ort { struct Env; struct Session; }

// One recognized piece of text with its axis-aligned box in image coordinates.
struct OcrDetection {
    std::string text; // UTF-8
    float score;      // recognition confidence, 0..1
    int x, y, w, h;
};

// Which script a text row must contain to count as a subtitle; rows without it
// (film credits, detector garbage) are dropped.  eAny keeps everything.
enum class OcrScriptFilter { eCjk, eLatin, eAny };

// PP-OCR text detection + recognition on ONNX Runtime, self-contained (no
// OpenCV): DBNet postprocessing is a threshold + connected components + box
// expansion, which is all subtitle-shaped text needs.  The recognizer's
// character dictionary is read from the model's embedded metadata.
class OcrEngine {
public:
    OcrEngine(Logger &log, const std::string &det_model_path, const std::string &rec_model_path);
    ~OcrEngine();

    OcrEngine(const OcrEngine &) = delete;
    OcrEngine &operator=(const OcrEngine &) = delete;

    // rgb is packed 8-bit RGB, width * height * 3 bytes.  Detections are
    // returned in the detector's order; use assembleLines() for reading order.
    std::vector<OcrDetection> recognize(const uint8_t *rgb, int width, int height);

    // Groups detections into text rows top-to-bottom (concatenating
    // side-by-side boxes left to right), dropping furigana ruby glosses
    // (boxes much shorter than the tallest) and rows failing the script
    // filter.  Mirrors tools/subocr/subocr.py.
    static std::vector<std::string> assembleLines(std::vector<OcrDetection> detections,
                                                  OcrScriptFilter filter);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Logger &m_log;
};

#endif // MUSELD_OCR_ENGINE_H
