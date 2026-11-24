// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_TVOS_GPUCHECK_H
#define MUSELD_TVOS_GPUCHECK_H

#include <string>

// tvOS only.  Whether this Apple TV's GPU can run museld's video pipeline:
// Metal's Apple 3 family (the A10X of the 2017 Apple TV 4K) or later.  The
// Apple TV HD's A8 is the Apple 2 family, which has no shared textures, and
// MoltenVK then produces a black picture (the tvOS simulator, also Apple 2,
// showed the same); there is no Info.plist key to keep the app off that
// model, so the app says so itself.  `name` gets the GPU's name for the log.
// MUSELD_FAKE_OLD_GPU=1 makes the check fail, to see the message.
namespace tvos {
    bool gpuCanRunMuseld(std::string &name);
}

#endif //MUSELD_TVOS_GPUCHECK_H
