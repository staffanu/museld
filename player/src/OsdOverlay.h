// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_OSDOVERLAY_H
#define MUSECPP_OSDOVERLAY_H

#include <string>

struct GLFWwindow;
class TextRenderer;
class Decoder;
struct PlayerState;
struct ResultImages;
namespace musevk { class CommandBuffer; }

class OsdOverlay {
public:
    // Returns the cursor coordinate string (empty if cursor overlay is off).
    std::string render(musevk::CommandBuffer &command_buffer,
                       ResultImages &images,
                       PlayerState &state,
                       const Decoder &decoder,
                       GLFWwindow *window,
                       TextRenderer &text_renderer);
};

#endif //MUSECPP_OSDOVERLAY_H
