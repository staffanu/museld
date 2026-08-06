// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cstdint>
#include <format>
#include "SubtitleFont.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "musevk/Size.h"
#include "musevk/VulkanUtil.h"

struct SubtitleFont::StbHolder {
    stbtt_fontinfo info;
};

std::vector<uint32_t> utf8ToCodepoints(const std::string &s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c0 = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int extra;
        if (c0 < 0x80) { ++i; out.push_back(c0); continue; }
        if      ((c0 & 0xE0) == 0xC0) { extra = 1; cp = c0 & 0x1F; }
        else if ((c0 & 0xF0) == 0xE0) { extra = 2; cp = c0 & 0x0F; }
        else if ((c0 & 0xF8) == 0xF0) { extra = 3; cp = c0 & 0x07; }
        else { ++i; out.push_back(0xFFFD); continue; }
        if (i + extra >= s.size()) { ++i; out.push_back(0xFFFD); continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            auto ck = static_cast<unsigned char>(s[i + k]);
            if ((ck & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (ok) { out.push_back(cp); i += extra + 1; }
        else    { out.push_back(0xFFFD); ++i; }
    }
    return out;
}

SubtitleFont::SubtitleFont(Logger &log,
                           const std::filesystem::path &ttf_path,
                           int pixel_height,
                           musevk::VulkanManager &vulkan_manager,
                           musevk::CommandPool & /*command_pool*/)
    : m_pixel_height(pixel_height),
      m_atlas_cpu(static_cast<size_t>(c_atlas_width) * c_atlas_height, 0),
      m_stb(std::make_unique<StbHolder>()),
      m_log(log),
      m_vulkan_manager(vulkan_manager) {
    std::ifstream in(ttf_path, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open font file: " + ttf_path.string());
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    m_font_data.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(m_font_data.data()), size);
    if (!in) throw std::runtime_error("Could not read font file: " + ttf_path.string());

    if (!stbtt_InitFont(&m_stb->info, m_font_data.data(),
                        stbtt_GetFontOffsetForIndex(m_font_data.data(), 0))) {
        throw std::runtime_error("stbtt_InitFont failed for: " + ttf_path.string());
    }

    m_scale = stbtt_ScaleForPixelHeight(&m_stb->info, static_cast<float>(pixel_height));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&m_stb->info, &asc, &desc, &gap);
    m_ascent = static_cast<int>(asc * m_scale + 0.5);
    m_descent = static_cast<int>(desc * m_scale - 0.5);
    m_line_gap = static_cast<int>(gap * m_scale + 0.5);
    m_line_height = m_ascent - m_descent + m_line_gap;

    // Reserve atlas slot 0 for the fallback glyph and ASCII space.
    rasterizeCodepoint(m_fallback_codepoint);
    rasterizeCodepoint(' ');
}

SubtitleFont::~SubtitleFont() = default;

// Horizontal compression applied to every glyph. Noto Sans JP has no condensed
// variant; rasterizing at a smaller x-scale than y-scale gives narrower text
// that's closer to typical broadcast subtitling without changing the font.
// Tweak this constant and rebuild.
static constexpr double c_width_scale = 0.85;

bool SubtitleFont::rasterizeCodepoint(uint32_t codepoint) {
    if (m_glyphs.count(codepoint)) return true;

    const int glyph_index = stbtt_FindGlyphIndex(&m_stb->info, static_cast<int>(codepoint));
    if (glyph_index == 0 && codepoint != m_fallback_codepoint) {
        // Lookups will resolve to the fallback '?'.  Live OCR text can contain
        // characters outside the font (e.g. simplified-Chinese variants from
        // the recognizer), so say which
        if (m_missing_logged.insert(codepoint).second)
            m_log.warn(eApplication,
                       std::format("Font has no glyph for U+{:04X}; showing '?'", codepoint));
        return false;
    }

    const double scale_x = m_scale * c_width_scale;
    const double scale_y = m_scale;

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&m_stb->info, glyph_index,
                            static_cast<float>(scale_x), static_cast<float>(scale_y),
                            &x0, &y0, &x1, &y1);
    int w = x1 - x0;
    int h = y1 - y0;

    int advance, lsb;
    stbtt_GetGlyphHMetrics(&m_stb->info, glyph_index, &advance, &lsb);
    const int adv_px = static_cast<int>(advance * scale_x + 0.5);

    GlyphInfo gi;
    gi.bearing_x = x0;
    gi.bearing_y = -y0; // distance from baseline up to top
    gi.advance = adv_px;
    gi.width = w;
    gi.height = h;

    if (w > 0 && h > 0) {
        // Shelf-pack into m_atlas_cpu.
        if (m_shelf_x + w + 1 > c_atlas_width) {
            m_shelf_x = 0;
            m_shelf_y += m_shelf_h + 1;
            m_shelf_h = 0;
        }
        if (m_shelf_y + h > c_atlas_height) {
            // Atlas full: render as empty so we still advance the pen correctly.
            // This shows up as blank gaps in the subtitles, so be loud about it.
            gi.width = 0;
            gi.height = 0;
            if (m_atlas_full_glyphs++ % 50 == 0)
                m_log.warn(eApplication,
                           std::format("Subtitle glyph atlas full: {} glyphs dropped so far "
                                       "(codepoint U+{:04X})", m_atlas_full_glyphs, codepoint));
        } else {
            gi.atlas_x = m_shelf_x;
            gi.atlas_y = m_shelf_y;
            stbtt_MakeGlyphBitmap(&m_stb->info,
                                  m_atlas_cpu.data() + gi.atlas_y * c_atlas_width + gi.atlas_x,
                                  w, h, c_atlas_width,
                                  static_cast<float>(scale_x), static_cast<float>(scale_y),
                                  glyph_index);
            m_shelf_x += w + 1;
            if (h > m_shelf_h) m_shelf_h = h;
            m_atlas_dirty = true;
        }
    }

    m_glyphs.emplace(codepoint, gi);
    return true;
}

void SubtitleFont::warmUpLine(const std::vector<uint32_t> &codepoints) {
    int line_ascent = 0;
    int line_descent = 0;
    bool any_glyph = false;
    for (uint32_t cp : codepoints) {
        rasterizeCodepoint(cp);
        auto it = m_glyphs.find(cp);
        if (it == m_glyphs.end()) it = m_glyphs.find(m_fallback_codepoint);
        if (it == m_glyphs.end()) continue;
        const GlyphInfo &g = it->second;
        if (g.width == 0 || g.height == 0) continue;
        any_glyph = true;
        if (g.bearing_y > line_ascent) line_ascent = g.bearing_y;
        const int below = g.height - g.bearing_y;
        if (below > line_descent) line_descent = below;
    }
    if (any_glyph) {
        m_line_ascents.push_back(line_ascent);
        m_line_descents.push_back(line_descent);
    }
}

void SubtitleFont::uploadAtlas(musevk::CommandPool &command_pool) {
    // Upload as a buffer of uint32_t (one pixel per element, low byte = coverage 0..255).
    // Wasteful 4x but matches the existing storage-buffer pattern used by TextRenderer,
    // and the GPU can index it directly with [y * width + x].
    std::vector<uint32_t> packed(m_atlas_cpu.size());
    for (size_t i = 0; i < m_atlas_cpu.size(); ++i) packed[i] = m_atlas_cpu[i];

    m_atlas_buffer = musevk::VulkanUtil::createDeviceBuffer(
        m_vulkan_manager, command_pool,
        musevk::Size(packed.size()), packed);
    m_atlas_dirty = false;
}

bool SubtitleFont::refreshAtlasIfDirty(musevk::CommandPool &command_pool) {
    if (!m_atlas_dirty || !m_atlas_buffer) return false;
    uploadAtlas(command_pool);
    return true;
}

void SubtitleFont::finalizeAtlas(musevk::CommandPool &command_pool) {
    uploadAtlas(command_pool);
    // The CPU-side atlas stays around: live OCR tracks warm up new glyphs
    // during playback, re-uploaded by refreshAtlasIfDirty()

    // Reference metrics: 90th percentile of per-line max ascent and descent.
    // Most Latin lines contain at least one descender (g/p/y/, etc.), so the
    // 90th percentile reflects a typical line's actual extent without being
    // dragged down by characters that have no descender, and without being
    // dragged up by a single rare tall outlier glyph on one line.
    auto percentile = [](std::vector<int> &v, double p) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        size_t idx = std::min(v.size() - 1, static_cast<size_t>(p * v.size()));
        return v[idx];
    };
    m_ref_ascent = percentile(m_line_ascents, 0.9);
    m_ref_descent = percentile(m_line_descents, 0.9);
}

LaidSubtitle SubtitleFont::layout(const std::vector<std::string> &lines,
                                  int frame_w, int frame_h, int max_width,
                                  bool anchor_top) const {
    LaidSubtitle out;

    // 1) Build the visible line set by soft-wrapping any line that doesn't fit.
    std::vector<std::vector<uint32_t>> visible;
    visible.reserve(lines.size());

    auto measure = [&](const std::vector<uint32_t> &cps, size_t from, size_t to) {
        int x = 0;
        for (size_t k = from; k < to; ++k) {
            auto it = m_glyphs.find(cps[k]);
            if (it == m_glyphs.end()) it = m_glyphs.find(m_fallback_codepoint);
            if (it != m_glyphs.end()) x += it->second.advance;
        }
        return x;
    };

    for (const auto &raw : lines) {
        auto cps = utf8ToCodepoints(raw);
        if (measure(cps, 0, cps.size()) <= max_width) {
            visible.push_back(std::move(cps));
            continue;
        }
        // Word-wrap on spaces.
        size_t start = 0;
        while (start < cps.size()) {
            size_t last_space = start;
            size_t end = start;
            int x = 0;
            while (end < cps.size()) {
                auto it = m_glyphs.find(cps[end]);
                if (it == m_glyphs.end()) it = m_glyphs.find(m_fallback_codepoint);
                int adv = (it != m_glyphs.end()) ? it->second.advance : 0;
                if (x + adv > max_width && end > start) break;
                x += adv;
                if (cps[end] == ' ') last_space = end;
                ++end;
            }
            size_t cut = (end < cps.size() && last_space > start) ? last_space : end;
            visible.emplace_back(cps.begin() + start, cps.begin() + cut);
            start = cut;
            while (start < cps.size() && cps[start] == ' ') ++start;
        }
    }

    if (visible.empty()) return out;

    const int pad_x = std::max(4, m_pixel_height / 6);
    const int pad_y = std::max(2, m_pixel_height / 10);
    // Visible gap between consecutive line plates, also used as the bottom
    // margin between the lowest plate and the bottom of the frame.
    const int inter_line_gap = std::max(2, m_pixel_height / 10);

    // Broadcast-style left justification: the block starts at 25 % of frame
    // width and is allowed to extend up to 90 %. If any single line would
    // exceed 65 % of the frame, the whole block shifts left by the overflow so
    // the longest line still ends at 90 %. All lines share the same start_x.
    const int normal_start_x = static_cast<int>(frame_w * 0.25);
    const int soft_limit = static_cast<int>(frame_w * 0.65);
    int max_line_width = 0;
    for (const auto &cps : visible) {
        int w = measure(cps, 0, cps.size());
        if (w > max_line_width) max_line_width = w;
    }
    const int overflow = std::max(0, max_line_width - soft_limit);
    const int block_start_x = std::max(pad_x, normal_start_x - overflow);

    // Lay out from the bottom up: the last visible line's plate bottom sits
    // inter_line_gap above the bottom of the frame, and each line above it is
    // placed so its plate bottom sits inter_line_gap above the next line's
    // plate top. This naturally accommodates per-line outlier glyphs without
    // a separate step calculation.
    int plate_bottom = frame_h - inter_line_gap;

    for (auto it_line = visible.rbegin(); it_line != visible.rend(); ++it_line) {
        const auto &cps = *it_line;

        const int line_width = measure(cps, 0, cps.size());
        const int pen_x_start = block_start_x;
        int pen_x = pen_x_start;

        // First pass over the line: collect per-glyph ascent / descent so we
        // know exactly how tall this line's plate needs to be before placing
        // its baseline.
        int line_ascent = m_ref_ascent;
        int line_descent = m_ref_descent;
        for (uint32_t cp : cps) {
            auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) it = m_glyphs.find(m_fallback_codepoint);
            if (it == m_glyphs.end()) continue;
            const GlyphInfo &g = it->second;
            if (g.width <= 0 || g.height <= 0) continue;
            if (g.bearing_y > line_ascent) line_ascent = g.bearing_y;
            int below = g.height - g.bearing_y;
            if (below > line_descent) line_descent = below;
        }

        const int baseline_y = plate_bottom - pad_y - line_descent;

        LaidLine ll;
        for (uint32_t cp : cps) {
            auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) it = m_glyphs.find(m_fallback_codepoint);
            if (it == m_glyphs.end()) continue;
            const GlyphInfo &g = it->second;
            if (g.width > 0 && g.height > 0) {
                LaidGlyph lg;
                lg.dst_x = pen_x + g.bearing_x;
                lg.dst_y = baseline_y - g.bearing_y;
                lg.atlas_x = g.atlas_x;
                lg.atlas_y = g.atlas_y;
                lg.width = g.width;
                lg.height = g.height;
                ll.glyphs.push_back(lg);
            }
            pen_x += g.advance;
        }

        ll.bg_x = pen_x_start - pad_x;
        ll.bg_y = baseline_y - line_ascent - pad_y;
        ll.bg_width = line_width + 2 * pad_x;
        ll.bg_height = line_ascent + line_descent + 2 * pad_y;
        if (ll.bg_x < 0) { ll.bg_width += ll.bg_x; ll.bg_x = 0; }
        if (ll.bg_y < 0) { ll.bg_height += ll.bg_y; ll.bg_y = 0; }
        if (ll.bg_x + ll.bg_width > frame_w) ll.bg_width = frame_w - ll.bg_x;
        if (ll.bg_y + ll.bg_height > frame_h) ll.bg_height = frame_h - ll.bg_y;

        // Next line up: its plate bottom is inter_line_gap above this plate's top.
        plate_bottom = ll.bg_y - inter_line_gap;

        out.lines.push_back(std::move(ll));
    }

    // Restore top-to-bottom order for callers / rendering.
    std::reverse(out.lines.begin(), out.lines.end());

    // The block is laid out bottom-anchored; for a top anchor, shift it so the
    // first plate's top sits inter_line_gap below the top of the frame.
    if (anchor_top && !out.lines.empty()) {
        const int shift = inter_line_gap - out.lines.front().bg_y;
        for (auto &ll : out.lines) {
            ll.bg_y += shift;
            for (auto &g : ll.glyphs)
                g.dst_y += shift;
        }
    }
    return out;
}
