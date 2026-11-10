// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_PLAYERACTIONS_H
#define MUSELD_PLAYERACTIONS_H

#include <string>
#include "AudioDefs.h"
#include "DropoutMode.h"
#include "InputController.h"
#include "PlayerState.h"

class Logger;
struct SDL_Window;

// What the player's keys do, as functions, so that the keys (InputController)
// and the menu (PlayerMenu) do the same things the same way.  Each sets the
// OSD text that names the new state, as the keys always have.  The cycling
// ones take a direction so the menu's left and right can step either way;
// the keys step forward.
namespace PlayerActions {
    void togglePause(PlayerState &state);
    void stepOneField(PlayerState &state);
    // Left/Right: pan while zoomed, else seek by `seconds`
    void seekOrPan(PlayerState &state, ReaderControls &reader, double seconds);
    // Up/Down: pan while zoomed, else the next or previous chapter
    void chapterOrPan(PlayerState &state, ReaderControls &reader, Logger &log, bool next);
    void togglePictureStops(PlayerState &state);
    void setFieldInterpolation(PlayerState &state, Logger &log, Decoder::FieldInterpolationMode mode);
    void cycleFieldInterpolation(PlayerState &state, Logger &log, int direction);
    void toggle3dComb(PlayerState &state, Logger &log);
    void toggleFilmMode(PlayerState &state, Logger &log);
    void cycleAudioTrack(PlayerState &state, ReaderControls &reader, AudioTrack &audio_track, int direction);
    void cycleAudioChannels(PlayerState &state, int direction);
    void cycleCx(PlayerState &state, const ReaderControls &reader, AudioTrack audio_track, int direction);
    void cycleBlackLevel(PlayerState &state, int direction);
    void cycleDropoutMode(PlayerState &state, DropoutMode &dropout_mode, int direction);
    void toggleNonLinear(PlayerState &state);
    void toggleCursor(PlayerState &state);
    void toggleDiscCode(PlayerState &state);
    void exportFrame(PlayerState &state);
    void cycleEqMode(PlayerState &state, ReaderControls &reader, Logger &log);
    void resetEq(PlayerState &state, ReaderControls &reader, Logger &log);
    void cycleEfmFilterSize(PlayerState &state, ReaderControls &reader, Logger &log, int direction);
    // slot: state.subtitle_primary or state.subtitle_secondary
    void cycleSubtitleSlot(PlayerState &state, int &slot, const char *osd_prefix, int direction);
    void cycleAspect(PlayerState &state, int direction);
    void cycleOverscan(PlayerState &state, int direction);
    void cycleZoom(PlayerState &state, int direction);
    void toggleFullScreen(SDL_Window *window, bool &full_screen, Logger &log);

    // Names for the menu's value column
    std::string fieldInterpolationName(Decoder::FieldInterpolationMode mode);
    std::string audioTrackName(const ReaderControls &reader, AudioTrack track);
    // false while the audio playing is multichannel (4-channel MUSE, 5.1 AC3/DTS), where
    // the channel choice is kept but not applied
    bool audioChannelChoiceHeard(const PlayerState &state);
    std::string audioChannelsName(const PlayerState &state);
    std::string cxName(const PlayerState &state, const ReaderControls &reader, AudioTrack audio_track);
    std::string blackLevelName(Decoder::BlackLevelMode mode);
    std::string dropoutModeName(DropoutMode mode);
    std::string efmFilterName(const ReaderControls &reader);
    std::string subtitleSlotName(const PlayerState &state, int slot);
    std::string overscanName(const PlayerState &state);
}

#endif //MUSELD_PLAYERACTIONS_H
