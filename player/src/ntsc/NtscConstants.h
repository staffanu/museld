// Copyright 2025-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_NTSCCONSTANTS_H
#define MUSECPP_NTSCCONSTANTS_H

// What the SD pipeline still takes from macros rather than from the
// VideoStandard it runs with (ntsc/VideoStandard.h, the C++ source of the
// geometry; shaders/muse/muse.h is the shaders' copy): the NTSC frame size for
// the NTSC-only film cadence tracker, and the standard-independent chroma halo.

#define NTSC_TOTAL_HEIGHT 525
#define NTSC_TOTAL_WIDTH 910
#define NTSC_CHROMA_TAP_HALO 9 // the chroma demodulation window reaches this far past the picture columns

#endif //MUSECPP_NTSCCONSTANTS_H
