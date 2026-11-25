// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_TVOS_THERMALSTATE_H
#define MUSELD_TVOS_THERMALSTATE_H

// tvOS only.  The system's thermal state (ProcessInfo.thermalState): nominal,
// fair, serious or critical.  The 2017 Apple TV 4K is passively cooled and
// slows its GPU when hot; the player logs a change of state beside its timing
// so that a slowdown can be told from a regression (2026-11-25: a nine-minute
// MUSE run was clean for three minutes and then dipped more and more often).
namespace tvos {
    const char *thermalStateName();
}

#endif //MUSELD_TVOS_THERMALSTATE_H
