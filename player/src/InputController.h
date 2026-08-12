// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTCONTROLLER_H
#define MUSECPP_INPUTCONTROLLER_H

#include <functional>
#include <set>
#include <string>

#include "AudioDefs.h"
#include "DropoutMode.h"

struct GLFWwindow;
struct PlayerState;
class Logger;

struct ReaderControls {
    std::function<void(double)> seek;
    std::function<void(AudioTrack)> setAudioTrack;
    // OSD label for the disc's default audio: the MUSE audio on MUSE discs,
    // the analog FM audio on NTSC discs
    std::string default_audio_label;
    // OSD label for the AC3-RF track; annotated in builds that cannot decode it
    std::string ac3_audio_label;
    // True when the default audio is the NTSC analog track, so the CX toggle
    // has an audible effect (unless another track is selected)
    bool has_analog_audio = false;
    // True when the disc format can carry an AC3-RF track (NTSC), which adds
    // it to the A key's cycle
    bool has_ac3_audio = false;
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
              AudioTrack &audio_track,
              bool &full_screen,
              int window_width,
              int window_height);

private:
    bool checkKey(GLFWwindow *window, int key);

    Logger &m_log;
    std::set<int> m_keys_down;
};

#endif //MUSECPP_INPUTCONTROLLER_H
