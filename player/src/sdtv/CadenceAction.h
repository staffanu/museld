// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_CADENCEACTION_H
#define MUSECPP_CADENCEACTION_H

// What the de-interlacer should do with a displayed field, as answered by
// the film cadence trackers (NtscCadenceTracker for 3:2, PalCadenceTracker
// for 2:2):
//   eAdaptive: no lock, or evidence against it -- per-pixel motion masks.
//   eWeave:    the previously decoded field belongs to the same film frame;
//              weave it in unconditionally.
//   eHold:     the field starts a new film frame whose other half is not
//              decoded yet; show the previous film frame once more (the
//              weave of the two previously decoded fields).
enum class CadenceFieldAction { eAdaptive, eWeave, eHold };

#endif //MUSECPP_CADENCEACTION_H
