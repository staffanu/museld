// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTCONTROLLER_H
#define MUSECPP_INPUTCONTROLLER_H

#include <functional>
#include <set>
#include <string>

#include "DropoutMode.h"

struct GLFWwindow;
struct PlayerState;
class Logger;

struct ReaderControls {
    std::function<void(double)> seek;
    std::function<void(bool)> setEfmEnabled;
    // Adaptive equalizer controls (MUSE only).  cycleEqMode returns a short label
    // for the new mode ("OFF"/"ADAPT"/"FROZEN") for OSD display.  Empty when the
    // decoder doesn't support adaptive equalization (NTSC).
    std::function<std::string()> cycleEqMode;
    std::function<void()> resetEqTaps;
};

class InputController {
public:
    explicit InputController(Logger &log) : m_log(log) {}

    // Returns false if the user requested quit.
    bool poll(GLFWwindow *window,
              PlayerState &state,
              ReaderControls &reader,
              DropoutMode &dropout_mode,
              bool &efm_audio,
              bool &full_screen,
              int window_width,
              int window_height);

private:
    bool checkKey(GLFWwindow *window, int key);

    Logger &m_log;
    std::set<int> m_keys_down;
};

#endif //MUSECPP_INPUTCONTROLLER_H
