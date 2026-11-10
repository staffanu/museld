// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

// SavedState::defaultPath, apart from the rest so the tests, which name their
// own file, need no SDL
#include <cstdlib>
#include <SDL3/SDL.h>
#include "SavedState.h"

std::string SavedState::defaultPath() {
    if (const char *env = getenv("MUSELD_STATE_FILE"); env != nullptr && *env)
        return env;
    char *pref = SDL_GetPrefPath("se.ulfberg", "museld");
    if (pref == nullptr)
        return "";
    std::string path = std::string(pref) + "state.txt";
    SDL_free(pref);
    return path;
}

