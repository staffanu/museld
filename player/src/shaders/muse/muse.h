// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#extension GL_EXT_shader_8bit_storage : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int8: enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : enable
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : enable

layout (local_size_x_id = 1) in;
layout (local_size_y_id = 2) in;
layout (local_size_z_id = 3) in;

#define MUSE_TOTAL_HEIGHT 1125
#define MUSE_TOTAL_WIDTH 480

#define MUSE_BUF_HEIGHT 516
#define MUSE_Y_BUF_WIDTH 374
#define MUSE_C_BUF_WIDTH 94
#define MUSE_C_OFFSET 11

// The SD (ntsc/) shaders are compiled twice: as ntsc_*.comp.spv with the
// NTSC geometry below, and as pal_*.comp.spv with -DPAL_GEOMETRY, where the
// same NTSC_* macros hold the PAL values (VideoStandard::pal() on the C++
// side).  The macro names stay NTSC_* so the shader bodies read the same.
#ifdef PAL_GEOMETRY
#define NTSC_TOTAL_HEIGHT 625
#define NTSC_TOTAL_WIDTH 1135
#define NTSC_DROPOUT_BIT_WORDS 36 // (NTSC_TOTAL_WIDTH + 31) / 32: one row of dropout flags as a bit mask
#define NTSC_Y_BUF_WIDTH 944
#define NTSC_FIELD_HEIGHT 288
#define NTSC_FIELD_START_X 180
#define NTSC_FIELD_START_Y 23
#define NTSC_FIELD2_OFFSET 313 // frame line of field 2's first line, less one
#define NTSC_BURST_START 102   // the colour burst window: 32 samples from here
#else
#define NTSC_TOTAL_HEIGHT 525
#define NTSC_TOTAL_WIDTH 910
#define NTSC_DROPOUT_BIT_WORDS 29 // (NTSC_TOTAL_WIDTH + 31) / 32: one row of dropout flags as a bit mask
#define NTSC_Y_BUF_WIDTH 764
#define NTSC_FIELD_HEIGHT 240
#define NTSC_FIELD_START_X 129
#define NTSC_FIELD_START_Y 22
#define NTSC_FIELD2_OFFSET 263
#define NTSC_BURST_START 78
#endif
#define NTSC_CHROMA_TAP_HALO 9 // the chroma demodulation window reaches this far past the picture columns
#define NTSC_CHROMA_TAPS_WIDTH (NTSC_Y_BUF_WIDTH + 2 * NTSC_CHROMA_TAP_HALO)
