// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEBLITTER_H
#define MUSECPP_FRAMEBLITTER_H

#include <vulkan/vulkan.hpp>

#include "DisplayGeometry.h"

struct GLFWwindow;
struct ResultImages;
class Logger;
namespace musevk { class CommandBuffer; class VulkanManager; }

class FrameBlitter {
public:
    // The shape of the monitor's pixels, width over height, from the
    // physical size and video mode the driver reports (1.0 for square
    // pixels, which is nearly always the answer; 1.0 too when the report
    // is missing or absurd).  Changes are logged.
    static double displayPixelAspect(GLFWwindow *window, Logger &log);

    // Clears the swap chain image and blits the source rectangle of the
    // decoded image into the destination rectangle
    void present(musevk::CommandBuffer &command_buffer,
                 ResultImages &images,
                 const DisplayGeometry &geometry,
                 vk::Image swap_chain_image,
                 vk::Semaphore image_available_semaphore);
};

#endif //MUSECPP_FRAMEBLITTER_H
