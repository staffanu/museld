// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEBLITTER_H
#define MUSECPP_FRAMEBLITTER_H

#include <vulkan/vulkan.hpp>

#include "DisplayGeometry.h"

struct SDL_Window;
struct ResultImages;
class Logger;
namespace musevk { class CommandBuffer; class VulkanManager; }

class FrameBlitter {
public:
    // The shape of the monitor's pixels, width over height, from the
    // physical size and video mode the driver reports.  SDL reports no
    // physical size, so this is 1.0 (square pixels), which was the answer on
    // every display tried when the EDID size was available; the display and
    // its mode are logged when they change, so the hook remains should a
    // display with non-square pixels ever turn up.
    static double displayPixelAspect(SDL_Window *window, Logger &log);

    // Clears the swap chain image and blits the source rectangle of the
    // decoded image into the destination rectangle
    void present(musevk::CommandBuffer &command_buffer,
                 ResultImages &images,
                 const DisplayGeometry &geometry,
                 vk::Image swap_chain_image,
                 vk::Semaphore image_available_semaphore);
};

#endif //MUSECPP_FRAMEBLITTER_H
