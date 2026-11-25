// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#import <Foundation/Foundation.h>
#include "ThermalState.h"

namespace tvos {
    const char *thermalStateName() {
        switch ([[NSProcessInfo processInfo] thermalState]) {
            case NSProcessInfoThermalStateNominal: return "nominal";
            case NSProcessInfoThermalStateFair: return "fair";
            case NSProcessInfoThermalStateSerious: return "serious";
            case NSProcessInfoThermalStateCritical: return "critical";
        }
        return "unknown";
    }
}
