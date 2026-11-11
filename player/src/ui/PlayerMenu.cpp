// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <format>
#include "PlayerMenu.h"
#include "PlayerActions.h"
#include "PlayerState.h"

namespace {

using namespace PlayerActions;

MenuItem setting(std::string label, std::string hint, std::function<std::string()> value, std::function<void(int)> step,
                 std::function<bool()> enabled = {}) {
    MenuItem item;
    item.label = std::move(label);
    item.hint = std::move(hint);
    item.value = std::move(value);
    item.step = std::move(step);
    item.enabled = std::move(enabled);
    return item;
}

MenuItem action(std::string label, std::string hint, std::function<void()> activate, std::function<bool()> enabled = {}) {
    MenuItem item;
    item.label = std::move(label);
    item.hint = std::move(hint);
    item.activate = std::move(activate);
    item.enabled = std::move(enabled);
    return item;
}

std::string onOff(bool on) { return on ? "ON" : "OFF"; }

} // namespace

MenuPage *buildPlayerMenu(Menu &menu, const PlayerMenuBindings &b, MenuPage *open_page) {
    PlayerState &s = b.state;
    ReaderControls &r = b.reader;
    Logger &log = b.log;
    const bool muse = b.is_muse, sd = !b.is_muse;

    MenuPage *root = menu.addPage("MUSELD");
    MenuPage *picture = menu.addPage("PICTURE");
    MenuPage *audio = menu.addPage("AUDIO");
    MenuPage *subtitles = menu.addPage("SUBTITLES");
    MenuPage *playback = menu.addPage("PLAYBACK");
    MenuPage *info = menu.addPage("INFORMATION");

    menu.addPageItem(root, "Picture", picture);
    menu.addPageItem(root, "Audio", audio);
    menu.addPageItem(root, "Subtitles", subtitles);
    menu.addPageItem(root, "Playback", playback);
    menu.addPageItem(root, "Information", info);
    if (open_page != nullptr)
        menu.addPageItem(root, "Open...", open_page);
    // Where the player returns to the browser (tvOS, a PC with --browse) this
    // ends the playback; started with a file on the command line it quits museld
    root->items.push_back(action(b.returns_to_browser ? "Stop" : "Quit museld", b.is_tvos ? "" : "Q",
                                 [b] { if (b.quit_request) *b.quit_request = true; }));

    // Picture
    picture->items.push_back(setting("Aspect", "F", [&s] { return std::string(aspectModeName(s.aspect_mode)); },
                                     [&s](int d) { cycleAspect(s, d); }));
    picture->items.push_back(setting("Overscan", "O", [&s] { return s.full_image ? std::string("OFF: FULL IMAGE") : std::format("{:g}%", s.overscan * 100); },
                                     [&s](int d) { cycleOverscan(s, d); }));
    picture->items.push_back(setting("Zoom", "Z", [&s] { return std::format("{}x", s.zoom_factor); },
                                     [&s](int d) { cycleZoom(s, d); }));
    picture->items.push_back(setting("Field interpolation", "1 2 3", [&s] {
                                         switch (s.field_interpolation_mode) {
                                             case Decoder::FieldInterpolationMode::eNormal: return "BY MOTION";
                                             case Decoder::FieldInterpolationMode::eForceIntraField: return "INTRA-FIELD";
                                             case Decoder::FieldInterpolationMode::eForceInterFrame: return "INTER-FRAME";
                                         }
                                         return "";
                                     },
                                     [&s, &log](int d) { cycleFieldInterpolation(s, log, d); }));
    picture->items.push_back(setting("3D comb", "4", [&s] { return onOff(s.use_3d_comb); },
                                     [&s, &log](int) { toggle3dComb(s, log); }, [sd] { return sd; }));
    picture->items.push_back(setting("Film mode", "5", [&s] { return s.film_mode ? "AUTO" : "OFF"; },
                                     [&s, &log](int) { toggleFilmMode(s, log); }, [sd] { return sd; }));
    picture->items.push_back(setting("Black level", "J", [&s] {
                                         return s.black_level_mode == Decoder::BlackLevelMode::eAuto ? "AUTO"
                                              : s.black_level_mode == Decoder::BlackLevelMode::eM ? "7.5 IRE (NTSC-M)" : "0 IRE (NTSC-J)";
                                     },
                                     [&s](int d) { cycleBlackLevel(s, d); }, [sd] { return sd; }));
    picture->items.push_back(setting("Dropouts", "D", [b] {
                                         return b.dropout_mode == DropoutMode::eNormal ? "CONCEALED"
                                              : b.dropout_mode == DropoutMode::eDisabled ? "SHOWN" : "HIGHLIGHTED";
                                     },
                                     [b](int d) { cycleDropoutMode(b.state, b.dropout_mode, d); }));
    picture->items.push_back(setting("Non-linear de-emphasis", "L", [&s] { return onOff(s.enable_non_linear); },
                                     [&s](int) { toggleNonLinear(s); }, [muse] { return muse; }));
    picture->items.push_back(setting("Equaliser", "7", [&r] { return r.eqMode ? r.eqMode() : std::string(); },
                                     [&s, &r, &log](int) { cycleEqMode(s, r, log); }, [&r] { return (bool)r.cycleEqMode; }));
    picture->items.push_back(action("Reset equaliser", "8", [&s, &r, &log] { resetEq(s, r, log); },
                                    [&r] { return (bool)r.resetEqTaps; }));
    picture->items.push_back(setting("Full screen", "TAB", [b] { return onOff(b.full_screen); },
                                     [b](int) { toggleFullScreen(b.window, b.full_screen, b.log); },
                                     [b] { return !b.is_tvos; }));

    // Audio
    audio->items.push_back(setting("Track", "A", [b] { return audioTrackName(b.reader, b.audio_track); },
                                   [b](int d) { cycleAudioTrack(b.state, b.reader, b.audio_track, d); }));
    // Hidden while multichannel audio plays (5.1 AC3/DTS, 4-channel MUSE): the
    // choice applies to two-channel audio only
    audio->items.push_back(setting("Channels", "B", [&s] {
                                       return s.audio_channel_mode == AudioChannelMode::eStereo ? "STEREO"
                                            : s.audio_channel_mode == AudioChannelMode::eLeft ? "LEFT" : "RIGHT";
                                   },
                                   [&s](int d) { cycleAudioChannels(s, d); },
                                   [&s] { return audioChannelChoiceHeard(s); }));
    audio->items.push_back(setting("CX expansion", "X", [b] {
                                       const char *name = b.state.analog_cx_mode == Decoder::CxMode::eAuto ? "AUTO"
                                                        : b.state.analog_cx_mode == Decoder::CxMode::eOff ? "OFF" : "ON";
                                       return b.audio_track == AudioTrack::eDefault ? std::string(name) : std::format("{} (NOT HEARD)", name);
                                   },
                                   [b](int d) { cycleCx(b.state, b.reader, b.audio_track, d); },
                                   [&r] { return r.has_analog_audio; }));
    audio->items.push_back(setting("EFM filter", "E", [&r] {
                                       const int size = r.efmFilterSize ? r.efmFilterSize() : 0;
                                       return size == 0 ? std::string("OFF") : std::format("{} TAPS", size);
                                   },
                                   [&s, &r, &log](int d) { cycleEfmFilterSize(s, r, log, d); },
                                   [&r] { return (bool)r.efmFilterSize && (bool)r.setEfmFilterSize; }));

    // Subtitles
    subtitles->items.push_back(setting("Subtitles", "[", [&s] { return subtitleSlotName(s, s.subtitle_primary); },
                                       [&s](int d) { cycleSubtitleSlot(s, s.subtitle_primary, "SUBTITLES", d); },
                                       [&s] { return !s.subtitle_track_names.empty(); }));
    subtitles->items.push_back(setting("Second subtitles", "]", [&s] { return subtitleSlotName(s, s.subtitle_secondary); },
                                       [&s](int d) { cycleSubtitleSlot(s, s.subtitle_secondary, "SUBTITLES 2", d); },
                                       [&s] { return !s.subtitle_track_names.empty(); }));
    subtitles->items.push_back(action("No subtitle tracks", "", [] {}, [&s] { return s.subtitle_track_names.empty(); }));

    // Playback
    playback->items.push_back(setting("Playing", "SPACE", [&s] { return s.paused ? "PAUSED" : "PLAYING"; },
                                      [&s](int) { togglePause(s); }));
    playback->items.push_back(action("Step one field", "N", [&s] { stepOneField(s); }));
    playback->items.push_back(action("Back 10 seconds", "LEFT", [&s, &r] { seekOrPan(s, r, -10); }));
    playback->items.push_back(action("Forward 10 seconds", "RIGHT", [&s, &r] { seekOrPan(s, r, 10); }));
    playback->items.push_back(action("Previous chapter", "DOWN", [&s, &r, &log] { chapterOrPan(s, r, log, false); }));
    playback->items.push_back(action("Next chapter", "UP", [&s, &r, &log] { chapterOrPan(s, r, log, true); }));
    playback->items.push_back(setting("Picture stops", "P", [&s] { return onOff(s.honor_picture_stops); },
                                      [&s](int) { togglePictureStops(s); }));

    // Information
    info->items.push_back(setting("Disc code", "V", [&s] { return onOff(s.show_disc_code); },
                                  [&s](int) { toggleDiscCode(s); }));
    info->items.push_back(setting("Pointer readout", "C", [&s] { return onOff(s.enable_cursor); },
                                  [&s](int) { toggleCursor(s); }, [b] { return !b.is_tvos; }));
    info->items.push_back(action("Export frame", "S", [&s] { exportFrame(s); }));
    menu.setRoot(root);
    return root;
}
