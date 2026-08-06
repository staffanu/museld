// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// Standalone check of the OCR engine against the Python prototype
// (tools/subocr): OCRs the subtitle band of one image and prints the lines.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "logging/StreamLogger.h"
#include "ocr/OcrEngine.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <det.onnx> <rec.onnx> <image.png> [band_fraction=0.28]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const double band_fraction = argc > 4 ? atof(argv[4]) : 0.28;

    StreamLogger log(StreamLogger::c_log_info, std::cerr, true);

    int width, height, channels;
    unsigned char *pixels = stbi_load(argv[3], &width, &height, &channels, 3);
    if (!pixels) {
        fprintf(stderr, "cannot load %s: %s\n", argv[3], stbi_failure_reason());
        return EXIT_FAILURE;
    }
    const int band_top = (int)((double)height * (1.0 - band_fraction));
    const unsigned char *band = pixels + (size_t)band_top * width * 3;
    const int band_height = height - band_top;

    OcrEngine engine(log, argv[1], argv[2]);
    auto detections = engine.recognize(band, width, band_height);
    for (const auto &d : detections)
        fprintf(stderr, "box (%d,%d %dx%d) score %.2f: %s\n", d.x, d.y, d.w, d.h, d.score,
                d.text.c_str());
    for (const auto &line : OcrEngine::assembleLines(detections, true))
        printf("%s\n", line.c_str());

    stbi_image_free(pixels);
    return EXIT_SUCCESS;
}
