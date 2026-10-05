// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTCONTROLLER_H
#define MUSECPP_INPUTCONTROLLER_H

#include <functional>
#include <set>
#include <string>

#include "AudioDefs.h"
#include "DropoutMode.h"

struct SDL_Window;
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
    // EFM timing recovery adaptive filter size (RF input only): the current
    // number of taps and a setter, empty when the reader has no EFM demodulator.
    std::function<int()> efmFilterSize;
    std::function<void(int)> setEfmFilterSize;
};

class InputController {
public:
    explicit InputController(Logger &log) : m_log(log) {}

    // Pumps the window system's events and acts on the keys pressed.  Must
    // run on the main thread.  Returns false if the user requested quit (a
    // key, or the window being closed).
    bool poll(SDL_Window *window,
              PlayerState &state,
              ReaderControls &reader,
              DropoutMode &dropout_mode,
              AudioTrack &audio_track,
              bool &full_screen);

private:
    // Whether the key (an SDL_Scancode: the physical key, whatever the
    // layout) was pressed since the last poll; key repeat does not count
    bool checkKey(int scancode) const;

    Logger &m_log;
    std::set<int> m_pressed; // scancodes pressed since the last poll
};

#endif //MUSECPP_INPUTCONTROLLER_H
