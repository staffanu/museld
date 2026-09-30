// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCCONSTANTS_H
#define MUSECPP_NTSCCONSTANTS_H


#define NTSC_TOTAL_HEIGHT 525
#define NTSC_TOTAL_WIDTH 910
#define NTSC_DROPOUT_BIT_WORDS 29 // (NTSC_TOTAL_WIDTH + 31) / 32, matches shaders/muse/muse.h

#define NTSC_FIELD_HEIGHT 240
#define NTSC_Y_BUF_WIDTH 764
#define NTSC_FIELD_START_X 129 // the 4 fsc sample after the sync edge in picture column 0; matches shaders/muse/muse.h
#define NTSC_CHROMA_TAP_HALO 9 // the chroma demodulation window reaches this far past the picture columns
#define NTSC_CHROMA_TAPS_WIDTH (NTSC_Y_BUF_WIDTH + 2 * NTSC_CHROMA_TAP_HALO)

#endif //MUSECPP_NTSCCONSTANTS_H
