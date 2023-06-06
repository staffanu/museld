//
// Created by staffanu on 6/2/23.
//

#extension GL_EXT_shader_16bit_storage : enable
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : enable

#ifndef NO_SET_WORKGROUP_SIZE
layout (local_size_x = 16) in;
layout (local_size_y = 1) in;
#endif

#define MUSE_INPUT_MULT 4.hf

#define MUSE_TOTAL_HEIGHT 1125
#define MUSE_TOTAL_WIDTH 480

#define MUSE_BUF_HEIGHT 516
#define MUSE_Y_BUF_WIDTH 374
#define MUSE_C_BUF_WIDTH 94
#define MUSE_C_OFFSET 11

#define MUSE_MOTION_FIELD_BUF_WIDTH (374/2)
