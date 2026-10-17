// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTCONTROLLER_H
#define MUSECPP_INPUTCONTROLLER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>

#include "AudioDefs.h"
#include "ChapterSearch.h"
#include "DropoutMode.h"

struct SDL_Window;
struct PlayerState;
class Logger;

struct ReaderControls {
    std::function<bool(double)> seek; // false: the input refused, nothing moved
    // Absolute positioning for the chapter search: returns the reader's new
    // seek generation, or nullopt when the input cannot be positioned
    std::function<std::optional<uint32_t>(int64_t)> seekToInputSample;
    double samples_per_second = 0;   // the input's sample rate
    // Its length in samples, -1 when unknown; a function because some
    // containers only give it up after a look at the file's tail
    std::function<int64_t()> input_sample_count;
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

// Begins a chapter search from the displayed frame: the next or previous
// chapter, or a given one.  Sets the OSD text and returns false when no
// search can start here (no chapter code, an input that cannot seek).
bool startChapterSearch(PlayerState &state, const ReaderControls &reader, Logger &log,
                        ChapterSearch::Direction direction, std::optional<int> target_chapter = std::nullopt);

class InputController {
public:
    // ignore_keys: poll only pumps the window events (--no-keys)
    explicit InputController(Logger &log, bool ignore_keys = false) : m_log(log), m_ignore_keys(ignore_keys) {}

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
    // The key a gamepad button (an SDL_GamepadButton) stands for, or 0
    static int gamepadButtonScancode(int button);

    Logger &m_log;
    const bool m_ignore_keys;
    std::set<int> m_pressed; // scancodes pressed since the last poll
};

#endif //MUSECPP_INPUTCONTROLLER_H
