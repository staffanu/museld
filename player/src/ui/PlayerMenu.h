// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_PLAYERMENU_H
#define MUSELD_PLAYERMENU_H

#include "Menu.h"
#include "AudioDefs.h"
#include "DropoutMode.h"

struct PlayerState;
struct ReaderControls;
class Logger;
struct SDL_Window;

// What the player's menu offers: the settings the keys set, as pages, bound
// to the player's state through PlayerActions so that a key and a menu item
// do the same thing.  The items follow the input: an item whose setting has
// no effect on the disc being played (the 3D comb on MUSE, the equaliser on
// NTSC) is not shown.
struct PlayerMenuBindings {
    PlayerState &state;
    ReaderControls &reader;
    DropoutMode &dropout_mode;
    AudioTrack &audio_track;
    bool &full_screen;
    SDL_Window *window;
    Logger &log;
    bool is_muse;       // the input is a MUSE disc (else NTSC or PAL)
    bool is_tvos;       // no full screen toggle, "Quit" leaves the app
    bool *quit_request; // set by the menu's Quit item
};

void buildPlayerMenu(Menu &menu, const PlayerMenuBindings &b);

#endif //MUSELD_PLAYERMENU_H
