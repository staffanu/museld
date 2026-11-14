// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_DISCSIGNATURE_H
#define MUSECPP_DISCSIGNATURE_H

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

class DiscInfo;

// The picture signature of a disc: for every frame read, the means of the
// frame buffer over a coarse grid of blocks, per field, with the disc's own
// position of that frame.  Two plays of one disc give the same numbers at the
// same disc frame whatever the decode settings, since the frame buffer is
// taken before the colour decoding, which is what lets a disc be recognized
// while it plays (to pick its subtitles).  This is the recording side only:
// --signature-write saves the frames of one playback in the order read.
//
// The file: a 64-byte header, then fixed-size records, all little-endian.
//   header   "LDSIG\0" | u16 version (1) | u8 standard | u8 fields | u8 rows | u8 columns
//            | u32 record size | u32 fps numerator | u32 fps denominator
//            | f32 value offset | f32 value scale | zeros
//   record   i32 disc frame | i16 chapter | u8 flags | u8 zero | u16 blocks[fields][rows][columns]
// A block value v of the frame buffer is stored as (v - offset) * scale.
namespace disc_signature {

constexpr int c_fields = 2, c_rows = 12, c_columns = 16;
constexpr int c_block_count = c_fields * c_rows * c_columns;
constexpr int c_header_size = 64;
constexpr int c_record_size = 8 + 2 * c_block_count;

enum class Standard : uint8_t { eNtsc = 0, ePal = 1, eMuse = 2 };

struct Header {
    Standard standard;
    uint32_t fps_num, fps_den;
    // The frame buffer's values are stored as (v - value_offset) * value_scale
    // in 16 bits: the SD buffers have blanking at 0 and white at 1, the MUSE
    // buffer its digital levels 0 to 255
    float value_offset, value_scale;

    bool operator==(const Header &) const = default;
    static Header forStandard(Standard standard, uint32_t fps_num, uint32_t fps_den);
};

// Record flags
constexpr uint8_t c_flag_clv_known = 1;        // the disc code has said whether the disc is CLV or CAV
constexpr uint8_t c_flag_clv = 2;
constexpr uint8_t c_flag_lead_in = 4;
constexpr uint8_t c_flag_lead_out = 8;
constexpr uint8_t c_flag_frame_resolution = 16; // disc_frame numbers the frames (unset: only whole seconds or minutes)

struct Frame {
    // The frame's position by the disc's own count: the CAV picture number
    // less one, or the CLV time code in frames (30 per code second on NTSC
    // and MUSE, 25 on PAL); -1 for a frame without a readable code
    int32_t disc_frame = -1;
    int16_t chapter = -1;
    uint8_t flags = 0;
    std::array<float, c_block_count> blocks{}; // [field][row][column], frame buffer values

    void setDiscInfo(const DiscInfo *info, uint32_t fps_num, uint32_t fps_den);
};

// Where a field's picture lies in the frame buffer (rows of `stride` samples)
struct FieldGeometry {
    int stride;
    int first_row[c_fields]; // 0-based row of each field's first picture line
    int rows;                // picture lines per field
    int first_column;        // 0-based
    int columns;
};

// The block means of a frame buffer of half floats: the grid covers the
// picture less a twentieth of its height at the top and bottom and a tenth
// of its width at the sides, which keeps the blanking edges, the overscan
// and whatever a player does at the borders out of the signature
void computeBlocks(const uint16_t *half_floats, const FieldGeometry &geometry,
                   std::array<float, c_block_count> &blocks);

class Writer {
public:
    // Throws std::runtime_error when the file cannot be created
    Writer(const std::string &path, const Header &header);
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    void append(const Frame &frame);
    [[nodiscard]] int64_t frameCount() const { return m_frames; }
    // False once a write has failed (disk full): the frames after it are lost
    [[nodiscard]] bool good() const { return m_good; }

private:
    Header m_header;
    FILE *m_file;
    int64_t m_frames = 0;
    bool m_good = true;
};

struct Recording {
    Header header;
    std::vector<Frame> frames;
};

// Reads a file back, the blocks as frame buffer values again (to the 16-bit
// step they were stored with).  Throws std::runtime_error on a file that is
// not a signature or of another version; a truncated last record is dropped.
Recording read(const std::string &path);

} // namespace disc_signature

#endif //MUSECPP_DISCSIGNATURE_H
