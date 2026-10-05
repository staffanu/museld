// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cmath>
#include <cstdint>
#include <format>
#include "FrameBlitter.h"

#include <SDL3/SDL.h>

#include "ResultImages.h"
#include "logging/Logger.h"
#include "musevk/CommandBuffer.h"
#include "musevk/VulkanImage.h"
#include "musevk/VulkanManager.h"

void FrameBlitter::present(musevk::CommandBuffer &command_buffer,
                           ResultImages &images,
                           const DisplayGeometry &geometry,
                           vk::Image swap_chain_image,
                           vk::Semaphore image_available_semaphore) {
    images.out_image->enqueueTransitionLayout(command_buffer, vk::ImageLayout::eTransferSrcOptimal,
                                              vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                                              vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead);
    command_buffer.enqueueTransitionMemoryLayout(swap_chain_image,
                                                 vk::ImageLayout::eUndefined,
                                                 vk::ImageLayout::eTransferDstOptimal,
                                                 vk::PipelineStageFlagBits::eTopOfPipe,
                                                 vk::PipelineStageFlagBits::eTransfer,
                                                 vk::AccessFlags(),
                                                 vk::AccessFlagBits::eTransferWrite);

    // The bars outside the picture (letterbox, pillarbox) are the cleared
    // background; the whole image is cleared since that is cheaper to
    // reason about than four bar rectangles and costs nothing noticeable
    command_buffer.enqueueClearColorImage(swap_chain_image, vk::ImageLayout::eTransferDstOptimal,
                                          vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 1.f}));
    command_buffer.enqueueBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite,
                                  vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
    vk::ImageBlit region;
    region.srcOffsets[0] = vk::Offset3D((int32_t)std::lround(geometry.src_x0), (int32_t)std::lround(geometry.src_y0), 0);
    region.srcOffsets[1] = vk::Offset3D((int32_t)std::lround(geometry.src_x1), (int32_t)std::lround(geometry.src_y1), 1);
    region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstOffsets[0] = vk::Offset3D(geometry.dst_x0, geometry.dst_y0, 0);
    region.dstOffsets[1] = vk::Offset3D(geometry.dst_x1, geometry.dst_y1, 1);
    region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    command_buffer.enqueueBlitImage(images.out_image->image(), vk::ImageLayout::eTransferSrcOptimal,
                                    swap_chain_image, vk::ImageLayout::eTransferDstOptimal,
                                    region);

    command_buffer.enqueueTransitionMemoryLayout(swap_chain_image,
                                                 vk::ImageLayout::eTransferDstOptimal,
                                                 vk::ImageLayout::ePresentSrcKHR,
                                                 vk::PipelineStageFlagBits::eTransfer,
                                                 vk::PipelineStageFlagBits::eBottomOfPipe,
                                                 vk::AccessFlagBits::eTransferWrite,
                                                 vk::AccessFlags());
    command_buffer.submit({image_available_semaphore}, {vk::PipelineStageFlagBits::eTopOfPipe}, {});
    command_buffer.wait();
}

double FrameBlitter::displayPixelAspect(SDL_Window *window, Logger &log) {
    // The display the window is on (full screen or not).  SDL does not report
    // a display's physical size, so the pixel aspect cannot be measured as it
    // was from the EDID through GLFW; it was 1.0 on every display tried
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0)
        return 1.0;
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
    const double aspect = 1.0;
    static SDL_DisplayID logged_display = 0;
    if (display != logged_display) {
        logged_display = display;
        const char *name = SDL_GetDisplayName(display);
        const char *driver = SDL_GetCurrentVideoDriver();
        log.info(eApplication | eVideo,
                 std::format("Display: {} {}x{} px at {:.4g} Hz, pixel aspect {:.3f} ({})", name != nullptr ? name : "?",
                             mode != nullptr ? mode->w : 0, mode != nullptr ? mode->h : 0,
                             mode != nullptr ? mode->refresh_rate : 0.0, aspect, driver != nullptr ? driver : "no video driver"));
    }
    return aspect;
}
