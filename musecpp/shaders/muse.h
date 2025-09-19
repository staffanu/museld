//
// Created by staffanu on 6/2/23.
//

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

#define NTSC_TOTAL_HEIGHT 525
#define NTSC_TOTAL_WIDTH 858
#define NTSC_Y_BUF_WIDTH 720
#define NTSC_FIELD_HEIGHT 241
