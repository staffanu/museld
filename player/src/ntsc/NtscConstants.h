// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCCONSTANTS_H
#define MUSECPP_NTSCCONSTANTS_H


#define NTSC_TOTAL_HEIGHT 525
#define NTSC_TOTAL_WIDTH 910
#define NTSC_DROPOUT_BIT_WORDS 29 // (NTSC_TOTAL_WIDTH + 31) / 32, matches shaders/muse/muse.h

#define NTSC_FIELD_HEIGHT 240
#define NTSC_Y_BUF_WIDTH 764

#endif //MUSECPP_NTSCCONSTANTS_H
