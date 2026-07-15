// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_SUBTITLE_FONT_H
#define MUSELD_SUBTITLE_FONT_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"

// Decode a UTF-8 std::string into a vector of Unicode codepoints. Malformed
// bytes are skipped. Used by the subtitle pipeline to convert SRT text into
// codepoints suitable for stb_truetype.
std::vector<uint32_t> utf8ToCodepoints(const std::string &s);

struct GlyphInfo {
    // Atlas rect of the rasterized bitmap (R8 coverage).
    int atlas_x = 0, atlas_y = 0;
    int width = 0, height = 0;
    // Bearings relative to the baseline / pen.
    int bearing_x = 0;
    int bearing_y = 0; // distance from baseline up to top of glyph (negative is below)
    // Horizontal advance to next pen position, in pixels.
    int advance = 0;
};

struct LaidGlyph {
    // Destination top-left in output-image pixels.
    int dst_x = 0;
    int dst_y = 0;
    // Source rect in the atlas.
    int atlas_x = 0;
    int atlas_y = 0;
    int width = 0;
    int height = 0;
};

struct LaidLine {
    std::vector<LaidGlyph> glyphs;
    // The visual rectangle to fill behind this line (a few pixels of padding around
    // the highest ascent / longest baseline-to-bottom).
    int bg_x = 0;
    int bg_y = 0;
    int bg_width = 0;
    int bg_height = 0;
};

struct LaidSubtitle {
    std::vector<LaidLine> lines;
};

// Owns a TTF font (parsed by stb_truetype) and a GPU-resident glyph-coverage atlas.
// Glyphs are rasterized lazily on the CPU into the atlas; the atlas is uploaded once
// in finalizeAtlas() before rendering. For v1 we pre-warm the atlas with every codepoint
// that appears in the SRT file at construction time, so finalizeAtlas() can be called
// once at startup and the atlas is then immutable.
class SubtitleFont {
public:
    static constexpr int c_atlas_width = 1024;
    static constexpr int c_atlas_height = 1024;

    // Throws std::runtime_error on failure.
    SubtitleFont(const std::filesystem::path &ttf_path,
                 int pixel_height,
                 musevk::VulkanManager &vulkan_manager,
                 musevk::CommandPool &command_pool);
    ~SubtitleFont();

    // Rasterize the codepoints of one displayed subtitle line, and record this
    // line's max ascent and descent for later percentile computation. Call this
    // once per logical line in the SRT (i.e. once per '\n'-separated row of each
    // cue). Cheap to call repeatedly: skips already-rasterized codepoints.
    void warmUpLine(const std::vector<uint32_t> &codepoints);

    // Upload the (currently CPU-side) atlas to GPU as a storage buffer. After this,
    // no further warmUp() calls should be made. Also computes reference ascent /
    // descent metrics (the 90th percentile over all rasterized glyphs) used to
    // size the background plates consistently across lines.
    void finalizeAtlas(musevk::CommandPool &command_pool);

    // Lay out one cue. `lines` is one string per visible line (line breaks come from
    // the SRT's own newlines). `max_width` is the wrapping width; lines wider than it
    // are word-wrapped. `frame_w` / `frame_h` set the anchor (bottom-center of frame).
    LaidSubtitle layout(const std::vector<std::string> &lines,
                        int frame_w, int frame_h, int max_width) const;

    [[nodiscard]] int pixelHeight() const { return m_pixel_height; }
    [[nodiscard]] int lineHeight() const { return m_line_height; }
    [[nodiscard]] int ascent() const { return m_ascent; }

    [[nodiscard]] std::shared_ptr<musevk::VulkanBuffer> atlasBuffer() const { return m_atlas_buffer; }
    [[nodiscard]] int atlasWidth() const { return c_atlas_width; }
    [[nodiscard]] int atlasHeight() const { return c_atlas_height; }

private:
    bool rasterizeCodepoint(uint32_t codepoint);

    int m_pixel_height;
    double m_scale = 0.0;
    int m_ascent = 0;
    int m_descent = 0;
    int m_line_gap = 0;
    int m_line_height = 0;

    // 90th-percentile of actual glyph ascents / descents over the rasterized set.
    // Used as the default vertical extent of the background plate.
    int m_ref_ascent = 0;
    int m_ref_descent = 0;

    // CPU-side atlas storage (R8 coverage). Uploaded once in finalizeAtlas().
    std::vector<uint8_t> m_atlas_cpu;
    // Shelf-packer state.
    int m_shelf_x = 0;
    int m_shelf_y = 0;
    int m_shelf_h = 0;

    std::unordered_map<uint32_t, GlyphInfo> m_glyphs;
    // Per-line max ascents and descents collected by warmUpLine(), used to
    // compute the reference (90th percentile) plate size in finalizeAtlas.
    // Per-line granularity matches the visual unit: a line is short of a
    // descender only if no character on it has one, which is rare in Latin
    // text, so most lines contribute a "normal" descent.
    std::vector<int> m_line_ascents;
    std::vector<int> m_line_descents;
    // Codepoint reserved for the "missing glyph" fallback (rasterized into slot 0).
    uint32_t m_fallback_codepoint = '?';

    // Opaque holder for stbtt_fontinfo to avoid leaking stb_truetype.h into the header.
    struct StbHolder;
    std::unique_ptr<StbHolder> m_stb;
    std::vector<uint8_t> m_font_data;

    musevk::VulkanManager &m_vulkan_manager;
    std::shared_ptr<musevk::VulkanBuffer> m_atlas_buffer;
};

#endif // MUSELD_SUBTITLE_FONT_H
