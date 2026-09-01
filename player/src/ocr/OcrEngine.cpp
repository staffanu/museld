// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "OcrEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <stdexcept>

#include <onnxruntime_cxx_api.h>

namespace {

// PP-OCR DBNet postprocessing constants (RapidOCR defaults)
constexpr float c_det_binarize_thresh = 0.3f;
constexpr float c_det_box_score_thresh = 0.5f;
constexpr float c_det_unclip_ratio = 1.6f;
constexpr int c_det_max_side = 960;   // longest side fed to the detector
constexpr int c_det_min_box_side = 3; // in probability-map pixels
constexpr int c_rec_height = 48;      // recognizer input height
constexpr int c_rec_max_width = 1280;
// Recognitions below this confidence are noise (scene content read as text);
// a single stray character needs to be much more convincing than a line
constexpr float c_rec_score_thresh = 0.6f;
constexpr float c_rec_single_char_score_thresh = 0.85f;

// Kana, kana marks, and CJK ideographs -- covers Japanese and Chinese subs
// (same ranges as JAPANESE_RE in tools/subocr/subocr.py)
bool isCjkCodepoint(uint32_t cp) {
    return (cp >= 0x3041 && cp <= 0x3096)    // hiragana
        || (cp >= 0x30a1 && cp <= 0x30fa)    // katakana
        || cp == 0x30fc || cp == 0x3005 || cp == 0x3006
        || (cp >= 0x4e00 && cp <= 0x9fff);   // CJK unified ideographs
}

bool isLatinLetterCodepoint(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
        || (cp >= 0xc0 && cp <= 0x24f && cp != 0xd7 && cp != 0xf7); // Latin-1..Extended-B
}

template<typename Predicate>
bool containsCodepoint(const std::string &utf8, Predicate predicate) {
    for (size_t i = 0; i < utf8.size();) {
        uint8_t b = utf8[i];
        uint32_t cp = 0;
        int len = 1;
        if (b < 0x80) { cp = b; }
        else if ((b & 0xe0) == 0xc0) { len = 2; cp = b & 0x1f; }
        else if ((b & 0xf0) == 0xe0) { len = 3; cp = b & 0x0f; }
        else if ((b & 0xf8) == 0xf0) { len = 4; cp = b & 0x07; }
        if (i + len > utf8.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | (utf8[i + k] & 0x3f);
        if (predicate(cp)) return true;
        i += len;
    }
    return false;
}

bool containsCjk(const std::string &utf8) {
    return containsCodepoint(utf8, isCjkCodepoint);
}

bool passesFilter(const std::string &utf8, OcrScriptFilter filter) {
    switch (filter) {
        case OcrScriptFilter::eCjk: return containsCjk(utf8);
        case OcrScriptFilter::eLatin: return containsCodepoint(utf8, isLatinLetterCodepoint);
        case OcrScriptFilter::eAny: return true;
    }
    return true;
}

// Bilinear resize of packed RGB into a caller-provided planar float buffer,
// normalized per channel as (x/255 - mean) / std.
void resizeToPlanarFloat(const uint8_t *rgb, int sw, int sh,
                         float *dst, int dw, int dh,
                         const std::array<float, 3> &mean, const std::array<float, 3> &stddev) {
    const float sx = (float)sw / (float)dw;
    const float sy = (float)sh / (float)dh;
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * sy - 0.5f;
        int y0 = std::clamp((int)std::floor(fy), 0, sh - 1);
        int y1 = std::min(y0 + 1, sh - 1);
        float wy = fy - (float)y0;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * sx - 0.5f;
            int x0 = std::clamp((int)std::floor(fx), 0, sw - 1);
            int x1 = std::min(x0 + 1, sw - 1);
            float wx = fx - (float)x0;
            for (int c = 0; c < 3; c++) {
                float v00 = rgb[(y0 * sw + x0) * 3 + c];
                float v01 = rgb[(y0 * sw + x1) * 3 + c];
                float v10 = rgb[(y1 * sw + x0) * 3 + c];
                float v11 = rgb[(y1 * sw + x1) * 3 + c];
                float v = v00 * (1 - wx) * (1 - wy) + v01 * wx * (1 - wy)
                        + v10 * (1 - wx) * wy + v11 * wx * wy;
                dst[(size_t)c * dw * dh + (size_t)y * dw + x] = (v / 255.0f - mean[c]) / stddev[c];
            }
        }
    }
}

} // namespace

struct OcrEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "museld-ocr"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    std::string det_input_name, det_output_name, rec_input_name, rec_output_name;
    // CTC classes: index 0 is the blank, then the model's characters, and the
    // last class is a space (the PaddleOCR use_space_char convention)
    std::vector<std::string> characters;
    Ort::MemoryInfo memory_info{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};

    static std::unique_ptr<Ort::Session> open(Ort::Env &env, Ort::SessionOptions &options,
                                              const std::string &path) {
        // ONNX Runtime takes wide paths on Windows (ORTCHAR_T is wchar_t
        // there); std::filesystem converts with the same native narrow
        // encoding the rest of museld's file handling uses.
        return std::make_unique<Ort::Session>(env, std::filesystem::path(path).c_str(), options);
    }

    std::vector<Ort::Value> run(Ort::Session &session, const std::string &in_name,
                                const std::string &out_name, std::vector<float> &data,
                                const std::array<int64_t, 4> &shape) {
        Ort::Value input = Ort::Value::CreateTensor<float>(
                memory_info, data.data(), data.size(), shape.data(), shape.size());
        const char *in_names[] = {in_name.c_str()};
        const char *out_names[] = {out_name.c_str()};
        return session.Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, 1);
    }
};

OcrEngine::OcrEngine(Logger &log, const std::string &det_model_path, const std::string &rec_model_path)
        : m_impl(std::make_unique<Impl>()), m_log(log) {
    m_impl->options.SetIntraOpNumThreads(2);
    m_impl->det = Impl::open(m_impl->env, m_impl->options, det_model_path);
    m_impl->rec = Impl::open(m_impl->env, m_impl->options, rec_model_path);

    Ort::AllocatorWithDefaultOptions alloc;
    m_impl->det_input_name = m_impl->det->GetInputNameAllocated(0, alloc).get();
    m_impl->det_output_name = m_impl->det->GetOutputNameAllocated(0, alloc).get();
    m_impl->rec_input_name = m_impl->rec->GetInputNameAllocated(0, alloc).get();
    m_impl->rec_output_name = m_impl->rec->GetOutputNameAllocated(0, alloc).get();

    Ort::ModelMetadata metadata = m_impl->rec->GetModelMetadata();
    Ort::AllocatedStringPtr dict = metadata.LookupCustomMetadataMapAllocated("character", alloc);
    if (!dict)
        throw std::runtime_error("recognizer model has no embedded 'character' dictionary");
    m_impl->characters.emplace_back(); // blank
    const char *p = dict.get();
    while (*p) {
        const char *nl = std::strchr(p, '\n');
        if (!nl) { m_impl->characters.emplace_back(p); break; }
        m_impl->characters.emplace_back(p, nl - p);
        p = nl + 1;
    }
    m_impl->characters.emplace_back(" ");

    const auto n_classes = m_impl->rec->GetOutputTypeInfo(0)
            .GetTensorTypeAndShapeInfo().GetShape().back();
    if (n_classes > 0 && (size_t)n_classes != m_impl->characters.size())
        m_log.warn(eApplication,
                   std::format("OCR dictionary has {} entries but the model has {} classes",
                               m_impl->characters.size(), n_classes));
    m_log.info(eApplication, std::format("OCR models loaded, {} character classes",
                                         m_impl->characters.size()));
}

OcrEngine::~OcrEngine() = default;

std::vector<OcrDetection> OcrEngine::recognize(const uint8_t *rgb, int width, int height) {
    // --- Detection: resize so the longest side is at most c_det_max_side,
    // rounded to multiples of 32 (slight aspect distortion, as PP-OCR does)
    float scale = std::min(1.0f, (float)c_det_max_side / (float)std::max(width, height));
    int dw = std::max(32, (int)std::round(width * scale / 32.0f) * 32);
    int dh = std::max(32, (int)std::round(height * scale / 32.0f) * 32);
    std::vector<float> det_input((size_t)3 * dw * dh);
    resizeToPlanarFloat(rgb, width, height, det_input.data(), dw, dh,
                        {0.485f, 0.456f, 0.406f}, {0.229f, 0.224f, 0.225f});

    auto det_out = m_impl->run(*m_impl->det, m_impl->det_input_name, m_impl->det_output_name,
                               det_input, {1, 3, dh, dw});
    const float *prob = det_out[0].GetTensorData<float>();
    auto prob_shape = det_out[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, 1, mh, mw]
    const int mh = (int)prob_shape[2], mw = (int)prob_shape[3];

    // --- DBNet postprocessing: binarize, connected components, expand boxes
    std::vector<uint8_t> visited((size_t)mh * mw, 0);
    std::vector<OcrDetection> detections;
    auto at = [&](int x, int y) { return prob[(size_t)y * mw + x]; };
    for (int sy = 0; sy < mh; sy++) {
        for (int sx = 0; sx < mw; sx++) {
            if (visited[(size_t)sy * mw + sx] || at(sx, sy) < c_det_binarize_thresh) continue;
            // BFS this component, accumulating its bounding box and score
            int min_x = sx, max_x = sx, min_y = sy, max_y = sy, count = 0;
            double score_sum = 0;
            std::deque<std::pair<int, int>> queue{{sx, sy}};
            visited[(size_t)sy * mw + sx] = 1;
            while (!queue.empty()) {
                auto [x, y] = queue.front();
                queue.pop_front();
                count++;
                score_sum += at(x, y);
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                constexpr int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) continue;
                    if (visited[(size_t)ny * mw + nx] || at(nx, ny) < c_det_binarize_thresh) continue;
                    visited[(size_t)ny * mw + nx] = 1;
                    queue.emplace_back(nx, ny);
                }
            }
            int bw = max_x - min_x + 1, bh = max_y - min_y + 1;
            if (bw < c_det_min_box_side || bh < c_det_min_box_side) continue;
            if (score_sum / count < c_det_box_score_thresh) continue;
            // DB's unclip: offset each side by area * ratio / perimeter
            float pad = (float)bw * bh * c_det_unclip_ratio / (2.0f * (bw + bh));
            float fx = (float)width / (float)mw, fy = (float)height / (float)mh;
            OcrDetection d;
            d.x = std::max(0, (int)(((float)min_x - pad) * fx));
            d.y = std::max(0, (int)(((float)min_y - pad) * fy));
            d.w = std::min(width, (int)(((float)max_x + 1 + pad) * fx)) - d.x;
            d.h = std::min(height, (int)(((float)max_y + 1 + pad) * fy)) - d.y;
            d.score = (float)(score_sum / count);
            if (d.w > 0 && d.h > 0) detections.push_back(d);
        }
    }

    // --- Recognition of each box crop
    for (auto &d : detections) {
        std::vector<uint8_t> crop((size_t)d.w * d.h * 3);
        for (int y = 0; y < d.h; y++)
            std::memcpy(&crop[(size_t)y * d.w * 3],
                        &rgb[((size_t)(d.y + y) * width + d.x) * 3], (size_t)d.w * 3);
        int rw = std::clamp((int)std::round((float)c_rec_height * d.w / d.h), 16, c_rec_max_width);
        std::vector<float> rec_input((size_t)3 * c_rec_height * rw);
        resizeToPlanarFloat(crop.data(), d.w, d.h, rec_input.data(), rw, c_rec_height,
                            {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f});
        auto rec_out = m_impl->run(*m_impl->rec, m_impl->rec_input_name, m_impl->rec_output_name,
                                   rec_input, {1, 3, c_rec_height, rw});
        const float *logits = rec_out[0].GetTensorData<float>();
        auto shape = rec_out[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, T, n_classes]
        const int steps = (int)shape[1];
        const int n_classes = (int)shape[2];

        // Greedy CTC decode: argmax per step, collapse repeats, drop blanks
        std::string text;
        double conf_sum = 0;
        int conf_count = 0, prev = 0;
        for (int t = 0; t < steps; t++) {
            const float *row = logits + (size_t)t * n_classes;
            int best = (int)(std::max_element(row, row + n_classes) - row);
            if (best != 0 && best != prev && best < (int)m_impl->characters.size()) {
                text += m_impl->characters[best];
                conf_sum += row[best];
                conf_count++;
            }
            prev = best;
        }
        d.text = std::move(text);
        d.score = conf_count ? (float)(conf_sum / conf_count) : 0.0f;
    }

    std::erase_if(detections, [](const OcrDetection &d) {
        if (d.text.empty() || d.score < c_rec_score_thresh) return true;
        // Count codepoints: a lone character must clear a higher bar
        size_t codepoints = 0;
        for (char c : d.text)
            codepoints += (c & 0xc0) != 0x80;
        return codepoints <= 1 && d.score < c_rec_single_char_score_thresh;
    });
    return detections;
}

std::vector<std::string> OcrEngine::assembleLines(std::vector<OcrDetection> detections,
                                                  OcrScriptFilter filter) {
    if (detections.empty()) return {};
    int max_h = 0;
    for (const auto &d : detections) max_h = std::max(max_h, d.h);
    std::erase_if(detections, [&](const OcrDetection &d) {
        if (d.h < 0.62f * (float)max_h) return true; // furigana ruby gloss
        return !passesFilter(d.text, filter);
    });
    std::sort(detections.begin(), detections.end(), [](const auto &a, const auto &b) {
        return a.y + a.h / 2 < b.y + b.h / 2;
    });

    // Cluster into rows by vertical position; concatenate left to right
    std::vector<std::vector<OcrDetection>> rows;
    int row_y = INT_MIN;
    for (auto &d : detections) {
        int yc = d.y + d.h / 2;
        if (rows.empty() || yc - row_y >= max_h / 2) {
            rows.emplace_back();
            row_y = yc;
        }
        rows.back().push_back(std::move(d));
    }
    std::vector<std::string> lines;
    for (auto &row : rows) {
        std::sort(row.begin(), row.end(), [](const auto &a, const auto &b) { return a.x < b.x; });
        // CJK text concatenates without separators; everything else gets
        // spaces between the detector's side-by-side fragments
        const bool all_cjk = std::all_of(row.begin(), row.end(), [](const auto &d) {
            return containsCjk(d.text);
        });
        std::string line;
        for (const auto &d : row) {
            if (!line.empty() && !all_cjk) line += ' ';
            line += d.text;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}
