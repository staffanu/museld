// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#import <Metal/Metal.h>
#include <cstdlib>
#include "GpuCheck.h"

namespace tvos {
    bool gpuCanRunMuseld(std::string &name) {
        @autoreleasepool {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device == nil) {
                name = "no Metal device";
                return false;
            }
            name = device.name.UTF8String;
            if (getenv("MUSELD_FAKE_OLD_GPU") != nullptr)
                return false;
            return [device supportsFamily:MTLGPUFamilyApple3];
        }
    }
}
