// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include "DiscSignature.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "DiscInfo.h"
#include "musevk/HalfFloatUtil.h"

namespace disc_signature {

namespace {

constexpr char c_magic[6] = {'L', 'D', 'S', 'I', 'G', 0};
constexpr uint16_t c_version = 1;

void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }
uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void putFloat(uint8_t *p, float v) { uint32_t u; std::memcpy(&u, &v, 4); put32(p, u); }
float getFloat(const uint8_t *p) { uint32_t u = get32(p); float v; std::memcpy(&v, &u, 4); return v; }

// Half floats to floats by table: a frame has a quarter of a million samples
// in its blocks
const float *halfTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; i++)
            t[i] = HalfFloatUtil::half_to_float((uint16_t)i);
        return t;
    }();
    return table.data();
}

} // namespace

Header Header::forStandard(Standard standard, uint32_t fps_num, uint32_t fps_den) {
    // SD: -0.25 to 1.75 of the blanking-to-white range; MUSE: levels 0 to 255.
    // Both leave the 16-bit step far below the noise of a block mean.
    return standard == Standard::eMuse ? Header{standard, fps_num, fps_den, 0.0f, 256.0f}
                                       : Header{standard, fps_num, fps_den, -0.25f, 32768.0f};
}

void Frame::setDiscInfo(const DiscInfo *info, uint32_t fps_num, uint32_t fps_den) {
    disc_frame = -1;
    chapter = -1;
    flags = 0;
    if (info == nullptr)
        return;
    // A second of the disc's time code is a whole number of frames
    const double frames_per_code_second = std::round((double)fps_num / fps_den);
    if (auto t = info->discTimeSeconds())
        disc_frame = (int32_t)std::llround(*t * frames_per_code_second);
    if (auto c = info->chapter())
        chapter = (int16_t)std::clamp(*c, 0, 32767);
    if (auto clv = info->isClv())
        flags |= c_flag_clv_known | (*clv ? c_flag_clv : 0);
    if (info->isLeadIn())
        flags |= c_flag_lead_in;
    if (info->isLeadOut())
        flags |= c_flag_lead_out;
    if (info->discTimeResolutionSeconds() < 0.5)
        flags |= c_flag_frame_resolution;
}

void computeBlocks(const uint16_t *half_floats, const FieldGeometry &g,
                   std::array<float, c_block_count> &blocks) {
    const float *table = halfTable();
    const int y_margin = g.rows / 20, x_margin = g.columns / 10;
    const int h = (g.rows - 2 * y_margin) / c_rows;
    const int w = (g.columns - 2 * x_margin) / c_columns;
    for (int field = 0; field < c_fields; field++)
        for (int r = 0; r < c_rows; r++) {
            double sums[c_columns] = {};
            for (int y = 0; y < h; y++) {
                const uint16_t *row = half_floats + (size_t)(g.first_row[field] + y_margin + r * h + y) * g.stride
                                      + g.first_column + x_margin;
                for (int c = 0; c < c_columns; c++) {
                    float sum = 0;
                    for (int x = 0; x < w; x++)
                        sum += table[row[c * w + x]];
                    sums[c] += sum;
                }
            }
            for (int c = 0; c < c_columns; c++)
                blocks[(field * c_rows + r) * c_columns + c] = (float)(sums[c] / (h * w));
        }
}

Writer::Writer(const std::string &path, const Header &header) : m_header(header), m_file(fopen(path.c_str(), "wb")) {
    if (m_file == nullptr)
        throw std::runtime_error("cannot create " + path);
    uint8_t h[c_header_size] = {};
    std::memcpy(h, c_magic, sizeof c_magic);
    put16(h + 6, c_version);
    h[8] = (uint8_t)header.standard;
    h[9] = c_fields;
    h[10] = c_rows;
    h[11] = c_columns;
    put32(h + 12, c_record_size);
    put32(h + 16, header.fps_num);
    put32(h + 20, header.fps_den);
    putFloat(h + 24, header.value_offset);
    putFloat(h + 28, header.value_scale);
    m_good = fwrite(h, 1, sizeof h, m_file) == sizeof h;
}

Writer::~Writer() {
    fclose(m_file);
}

void Writer::append(const Frame &frame) {
    if (!m_good)
        return;
    uint8_t r[c_record_size];
    put32(r, (uint32_t)frame.disc_frame);
    put16(r + 4, (uint16_t)frame.chapter);
    r[6] = frame.flags;
    r[7] = 0;
    for (int i = 0; i < c_block_count; i++) {
        const float v = (frame.blocks[i] - m_header.value_offset) * m_header.value_scale;
        put16(r + 8 + 2 * i, (uint16_t)std::lround(std::clamp(v, 0.0f, 65535.0f)));
    }
    m_good = fwrite(r, 1, sizeof r, m_file) == sizeof r;
    if (m_good)
        m_frames++;
}

Recording read(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        throw std::runtime_error("cannot open " + path);
    struct Closer { FILE *f; ~Closer() { fclose(f); } } closer{f};
    uint8_t h[c_header_size];
    if (fread(h, 1, sizeof h, f) != sizeof h || std::memcmp(h, c_magic, sizeof c_magic) != 0)
        throw std::runtime_error(path + " is not a disc signature");
    if (get16(h + 6) != c_version || h[8] > (uint8_t)Standard::eMuse || h[9] != c_fields || h[10] != c_rows
        || h[11] != c_columns || get32(h + 12) != c_record_size)
        throw std::runtime_error(path + " is a disc signature of a version this build does not read");
    Recording out;
    out.header = Header{(Standard)h[8], get32(h + 16), get32(h + 20), getFloat(h + 24), getFloat(h + 28)};
    uint8_t r[c_record_size];
    while (fread(r, 1, sizeof r, f) == sizeof r) {
        Frame frame;
        frame.disc_frame = (int32_t)get32(r);
        frame.chapter = (int16_t)get16(r + 4);
        frame.flags = r[6];
        for (int i = 0; i < c_block_count; i++)
            frame.blocks[i] = get16(r + 8 + 2 * i) / out.header.value_scale + out.header.value_offset;
        out.frames.push_back(frame);
    }
    return out;
}

} // namespace disc_signature
