// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSELD_MENUOVERLAY_H
#define MUSELD_MENUOVERLAY_H

struct PlayerState;
struct ResultImages;
class Menu;
class TextRenderer;
namespace musevk { class CommandBuffer; }

// Draws the open menu into the decoded frame, over the picture, like the
// OSD: a darkened panel at the left with the page title, the items in a
// column with their values at the right, and a bar under the selected one.
// Everything is drawn in the frame's own pixels, so the glyph scale follows
// the frame height as the OSD's does.
class MenuOverlay {
public:
    void render(musevk::CommandBuffer &command_buffer, ResultImages &images, PlayerState &state,
                const Menu &menu, TextRenderer &text_renderer);
};

#endif //MUSELD_MENUOVERLAY_H
