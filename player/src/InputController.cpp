// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "InputController.h"
#include "PlayerActions.h"
#include "ui/Menu.h"
#include "ui/PlayerMenu.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <SDL3/SDL.h>

#include "PlayerState.h"
#include "logging/Logger.h"

PositionSearch::Reading positionReading(const PlayerState &state) {
    PositionSearch::Reading reading{state.last_decoded.disc_info_input_offset};
    if (const auto &info = state.last_decoded.disc_info) {
        reading.chapter = info->chapter();
        reading.time = info->discTimeSeconds();
        reading.time_resolution = info->discTimeResolutionSeconds();
        reading.frame = info->frameNumber();
        reading.lead_in = info->isLeadIn();
        reading.lead_out = info->isLeadOut();
    }
    return reading;
}

bool startPositionSearch(PlayerState &state, const ReaderControls &reader, Logger &log,
                         const PositionSearch::Target &target) {
    if (state.position_search)
        return false; // one at a time
    if (!reader.seekToInputSample || reader.samples_per_second <= 0) {
        state.osd_text = "SEARCH NOT AVAIL";
        return false;
    }
    const PositionSearch::Reading here = positionReading(state);
    PositionSearch::Params params{reader.samples_per_second,
                                  reader.input_sample_count ? reader.input_sample_count() : -1};
    if (reader.frames_per_second > 0)
        params.frames_per_second = reader.frames_per_second;
    auto search = std::make_unique<PositionSearch>(params, here, target);
    if (search->action().kind == PositionSearch::Action::Kind::eFailed) {
        // No code to start from (or no input length): said at once, nothing moves
        state.osd_text = search->action().reason;
        return false;
    }
    if (search->action().kind == PositionSearch::Action::Kind::eDone) {
        state.osd_text = PositionSearch::describe(target.kind, search->action().value); // already there
        return true;
    }
    state.position_search = std::move(search);
    state.search_action_pending = true;
    // The search needs the decoder running; back to the pause when done.
    // The OSD text is drawn into the displayed image, which the search
    // leaves as it is, so re-decode it once first: that clears whatever
    // text is on it before "SEARCH" goes on.
    state.search_resume_paused = state.paused;
    state.paused = false;
    state.redo_last_field = true;
    state.paused_countdown = 0;
    using Kind = PositionSearch::Target::Kind;
    log.info(eApplication, std::format("Position search: {} from {} at input sample {}",
                                       target.kind == Kind::eNextChapter ? "next chapter"
                                       : target.kind == Kind::ePreviousChapter ? "previous chapter"
                                       : PositionSearch::describe(target.kind, target.value),
                                       PositionSearch::describe(target.kind, *PositionSearch::coordinate(here, target.kind)),
                                       here.offset));
    return true;
}

bool InputController::checkKey(int scancode) const {
    return m_pressed.contains(scancode);
}

// Gamepad buttons act as keys.  The Siri Remote is SDL's micro gamepad:
// select is SOUTH, play/pause is WEST, Menu is EAST, the touch surface the
// d-pad; a game controller adds the rest.
int InputController::gamepadButtonScancode(int button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          // select / A
        case SDL_GAMEPAD_BUTTON_WEST:           // play-pause / X
        case SDL_GAMEPAD_BUTTON_START:
            return SDL_SCANCODE_SPACE;
        case SDL_GAMEPAD_BUTTON_EAST:           // Menu / B
        case SDL_GAMEPAD_BUTTON_BACK:
            return SDL_SCANCODE_ESCAPE;
        case SDL_GAMEPAD_BUTTON_NORTH:          // Y
            return SDL_SCANCODE_A;              // audio track
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return SDL_SCANCODE_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return SDL_SCANCODE_RIGHT;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return SDL_SCANCODE_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return SDL_SCANCODE_DOWN;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return SDL_SCANCODE_LEFTBRACKET;    // subtitle tracks
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return SDL_SCANCODE_RIGHTBRACKET;
        default:
            return 0;
    }
}

void InputController::discardPending() {
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    SDL_FlushEvents(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP);
    m_pressed.clear();
}

bool InputController::poll(SDL_Window *window,
                           PlayerState &state,
                           ReaderControls &reader,
                           DropoutMode &dropout_mode,
                           AudioTrack &audio_track,
                           bool &full_screen) {
    // Drain the event queue: the keys pressed since the last poll, and whether
    // the window was closed.  Scancodes name the physical key, so the
    // bindings sit where they do on a US keyboard whatever the layout.
    m_pressed.clear();
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_TERMINATING:
                m_log.debug(eApplication, std::format("Quit event {:#x}", (unsigned)event.type));
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN: {
                // Held arrows repeat in the menu (a long listing), and a held Backspace
                // in its address field; other keys act once per press
                const bool arrow = event.key.scancode == SDL_SCANCODE_UP || event.key.scancode == SDL_SCANCODE_DOWN
                                   || event.key.scancode == SDL_SCANCODE_LEFT || event.key.scancode == SDL_SCANCODE_RIGHT;
                const bool menu_open = m_menu != nullptr && m_menu->isOpen();
                const bool erasing = event.key.scancode == SDL_SCANCODE_BACKSPACE && menu_open
                                     && m_menu->entry() != nullptr;
                if (!event.key.repeat || (arrow && menu_open) || erasing) {
                    m_pressed.insert(event.key.scancode);
                    if (!event.key.repeat)
                        m_log.debug(eApplication, std::format("Key: {}", SDL_GetScancodeName(event.key.scancode)));
                }
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
                // A keyboard typing into the menu's text entry (the address field).
                // Not a space: the space bar picks the key under the cursor on the
                // on-screen keyboard (below, as a gamepad's select does), and the
                // keyboard's SPACE key there types one
                if (m_menu != nullptr && m_menu->isOpen() && m_menu->entry() != nullptr
                    && strcmp(event.text.text, " ") != 0)
                    m_menu->type(event.text.text);
                break;
            // Gamepads deliver button events only once opened
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *gamepad = SDL_OpenGamepad(event.gdevice.which))
                    m_log.info(eApplication, std::format("Gamepad: {}", SDL_GetGamepadName(gamepad)));
                else
                    m_log.warn(eApplication, std::format("SDL_OpenGamepad: {}", SDL_GetError()));
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event.gdevice.which))
                    SDL_CloseGamepad(gamepad);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                const int scancode = gamepadButtonScancode(event.gbutton.button);
                if (scancode)
                    m_pressed.insert(scancode);
                m_log.debug(eApplication, std::format("Gamepad button: {} -> {}",
                                                      SDL_GetGamepadStringForButton((SDL_GamepadButton)event.gbutton.button),
                                                      scancode ? SDL_GetScancodeName((SDL_Scancode)scancode) : "nothing"));
                break;
            }
            // tvOS: the app leaves the screen (Menu, the TV button, a call).
            // Pause, and the main loop stops presenting until it is back.
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                if (!state.in_background) {
                    state.in_background = true;
                    state.paused_before_background = state.paused;
                    state.paused = true;
                }
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                if (state.in_background) {
                    state.in_background = false;
                    state.paused = state.paused_before_background;
                }
                break;
            default:
                break;
        }
    }
    if (quit)
        return false;
    if (m_ignore_keys)
        return true; // --no-keys: the events are pumped, the keys do nothing
    // The menu, when open, takes every key: the arrows move in it, select
    // steps or activates, M (or the Menu button, which arrives as Escape) and
    // Escape go back and close it.  Quitting the program therefore means
    // leaving the menu first.  Closed, M opens it; on tvOS, where the app
    // starts in the menu and has no command line to fall back on, the Menu
    // button opens it and, from the menu's root, leaves the app.
#if defined(__APPLE__) && TARGET_OS_TV
    constexpr bool tvos = true;
#else
    constexpr bool tvos = false;
#endif
    // The window takes text input while the menu's address field is up, so a
    // keyboard types into it (and its letters are not hotkeys meanwhile)
    const bool entry_up = m_menu != nullptr && m_menu->isOpen() && m_menu->entry() != nullptr;
    // Not on tvOS: SDL's text input there is the system's full-screen keyboard
    // dialog, which would come up over the menu's own keyboard (the one the
    // remote drives, with the address prefilled)
    if (!tvos && entry_up != m_text_input_on) {
        m_text_input_on = entry_up;
        if (entry_up)
            SDL_StartTextInput(window);
        else
            SDL_StopTextInput(window);
    }
    if (entry_up) {
        if (checkKey(SDL_SCANCODE_UP)) m_menu->handleKey(MenuKey::eUp);
        if (checkKey(SDL_SCANCODE_DOWN)) m_menu->handleKey(MenuKey::eDown);
        if (checkKey(SDL_SCANCODE_LEFT)) m_menu->handleKey(MenuKey::eLeft);
        if (checkKey(SDL_SCANCODE_RIGHT)) m_menu->handleKey(MenuKey::eRight);
        // Space picks the key under the cursor on the on-screen keyboard.  Return
        // too on tvOS, where the remote's select arrives as Return; on a PC a
        // keyboard's Return accepts what was typed and Escape cancels
        if (checkKey(SDL_SCANCODE_SPACE)) m_menu->handleKey(MenuKey::eSelect);
        if (checkKey(SDL_SCANCODE_RETURN) || checkKey(SDL_SCANCODE_KP_ENTER)) {
            if (tvos)
                m_menu->handleKey(MenuKey::eSelect);
            else
                m_menu->acceptEntry();
        }
        if (checkKey(SDL_SCANCODE_BACKSPACE)) m_menu->backspace();
        if (checkKey(SDL_SCANCODE_ESCAPE)) {
            if (tvos)
                m_menu->handleKey(MenuKey::eBack); // the Menu button: delete, cancel when empty
            else
                m_menu->cancelEntry();
        }
        return true;
    }
    if (m_menu != nullptr && m_menu->isOpen()) {
        if (checkKey(SDL_SCANCODE_UP)) m_menu->handleKey(MenuKey::eUp);
        if (checkKey(SDL_SCANCODE_DOWN)) m_menu->handleKey(MenuKey::eDown);
        if (checkKey(SDL_SCANCODE_LEFT)) m_menu->handleKey(MenuKey::eLeft);
        if (checkKey(SDL_SCANCODE_RIGHT)) m_menu->handleKey(MenuKey::eRight);
        if (checkKey(SDL_SCANCODE_SPACE) || checkKey(SDL_SCANCODE_RETURN)) m_menu->handleKey(MenuKey::eSelect);
        if (checkKey(SDL_SCANCODE_M) && !m_menu_is_screen) {
            m_menu->close();
        } else if (checkKey(SDL_SCANCODE_ESCAPE) || checkKey(SDL_SCANCODE_BACKSPACE)) {
            if (!m_menu->handleKey(MenuKey::eBack)) { // on the root page
                // Back from the root closes the menu, back to the picture; only
                // the browser screen, with nothing beneath it, is left this way.
                // Leaving playback is the menu's own Stop/Quit item (a TV remote
                // over HDMI-CEC has the arrows, select and back, nothing else)
                if (m_menu_is_screen) {
                    m_log.debug(eApplication, "Back from the menu's root: leaving");
                    return false;
                }
                m_menu->close();
            }
        }
        return true;
    }
    if (m_menu != nullptr && (checkKey(SDL_SCANCODE_M) || (tvos && checkKey(SDL_SCANCODE_ESCAPE)))) {
        m_menu->open();
        return true;
    }
    if (checkKey(SDL_SCANCODE_ESCAPE) || checkKey(SDL_SCANCODE_Q))
        return false;
    if (checkKey(SDL_SCANCODE_TAB))
        PlayerActions::toggleFullScreen(window, full_screen, m_log);
    // Return as well as Space: on tvOS a remote's select and play/pause
    // buttons (an HDMI-CEC remote, or the Siri Remote through tvOS's own
    // translation) arrive as Return key presses
    if (checkKey(SDL_SCANCODE_SPACE) || checkKey(SDL_SCANCODE_RETURN))
        PlayerActions::togglePause(state);
    if (checkKey(SDL_SCANCODE_N))
        PlayerActions::stepOneField(state);
    // Left/Right seek, Up/Down find the next/previous chapter (see
    // PositionSearch; the loop in runPlayer drives it); all four pan instead
    // while zoomed.  G asks for a time or frame to go to.
    if (checkKey(SDL_SCANCODE_LEFT))
        PlayerActions::seekOrPan(state, reader, -10);
    if (checkKey(SDL_SCANCODE_RIGHT))
        PlayerActions::seekOrPan(state, reader, 10);
    if (checkKey(SDL_SCANCODE_UP))
        PlayerActions::chapterOrPan(state, reader, m_log, true);
    if (checkKey(SDL_SCANCODE_DOWN))
        PlayerActions::chapterOrPan(state, reader, m_log, false);
    if (checkKey(SDL_SCANCODE_G) && m_menu != nullptr)
        openGoToEntry(*m_menu, state, reader, m_log);
    if (checkKey(SDL_SCANCODE_P))
        PlayerActions::togglePictureStops(state);
    if (checkKey(SDL_SCANCODE_1))
        PlayerActions::setFieldInterpolation(state, m_log, Decoder::FieldInterpolationMode::eNormal);
    if (checkKey(SDL_SCANCODE_2))
        PlayerActions::setFieldInterpolation(state, m_log, Decoder::FieldInterpolationMode::eForceIntraField);
    if (checkKey(SDL_SCANCODE_3))
        PlayerActions::setFieldInterpolation(state, m_log, Decoder::FieldInterpolationMode::eForceInterFrame);
    if (checkKey(SDL_SCANCODE_4))
        PlayerActions::toggle3dComb(state, m_log);
    if (checkKey(SDL_SCANCODE_5))
        PlayerActions::toggleFilmMode(state, m_log);
    if (checkKey(SDL_SCANCODE_6))
        PlayerActions::cycleTemporalNr(state, m_log, +1);
    if (checkKey(SDL_SCANCODE_A))
        PlayerActions::cycleAudioTrack(state, reader, audio_track, +1);
    if (checkKey(SDL_SCANCODE_B))
        PlayerActions::cycleAudioChannels(state, +1);
    if (checkKey(SDL_SCANCODE_X))
        PlayerActions::cycleCx(state, reader, audio_track, +1);
    if (checkKey(SDL_SCANCODE_J))
        PlayerActions::cycleBlackLevel(state, +1);
    if (checkKey(SDL_SCANCODE_D))
        PlayerActions::cycleDropoutMode(state, dropout_mode, +1);
    if (checkKey(SDL_SCANCODE_L))
        PlayerActions::toggleNonLinear(state);
    if (checkKey(SDL_SCANCODE_C))
        PlayerActions::toggleCursor(state);
    if (checkKey(SDL_SCANCODE_V))
        PlayerActions::toggleDiscCode(state);
    if (checkKey(SDL_SCANCODE_PRINTSCREEN))
        SDL_SetClipboardText(state.last_cursor_string.c_str());
    if (checkKey(SDL_SCANCODE_S))
        PlayerActions::exportFrame(state);
    if (checkKey(SDL_SCANCODE_7))
        PlayerActions::cycleEqMode(state, reader, m_log);
    if (checkKey(SDL_SCANCODE_8))
        PlayerActions::resetEq(state, reader, m_log);
    if (checkKey(SDL_SCANCODE_E))
        PlayerActions::cycleEfmFilterSize(state, reader, m_log, +1);
    if (checkKey(SDL_SCANCODE_LEFTBRACKET))
        PlayerActions::cycleSubtitleSlot(state, state.subtitle_primary, "SUBTITLES", +1);
    if (checkKey(SDL_SCANCODE_RIGHTBRACKET))
        PlayerActions::cycleSubtitleSlot(state, state.subtitle_secondary, "SUBTITLES 2", +1);
    if (checkKey(SDL_SCANCODE_F))
        PlayerActions::cycleAspect(state, +1);
    if (checkKey(SDL_SCANCODE_O))
        PlayerActions::cycleOverscan(state, +1);
    if (checkKey(SDL_SCANCODE_Z))
        PlayerActions::cycleZoom(state, +1);
    return true;
}
