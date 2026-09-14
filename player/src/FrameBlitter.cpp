// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <cmath>
#include <cstdint>
#include <format>
#include "FrameBlitter.h"

#include <GLFW/glfw3.h>

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
    region.srcOffsets[0] = vk::Offset3D((int32_t)geometry.src_x0, (int32_t)geometry.src_y0, 0);
    region.srcOffsets[1] = vk::Offset3D((int32_t)geometry.src_x1, (int32_t)geometry.src_y1, 1);
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

double FrameBlitter::displayPixelAspect(GLFWwindow *window, Logger &log) {
    // Full screen: the monitor shown on.  Windowed: the primary monitor --
    // finding the one the window is actually on needs the window position,
    // which Wayland does not give out, and the answer is 1.0 on any monitor
    // in practice; the physical size matters for TVs and odd video modes
    GLFWmonitor *monitor = glfwGetWindowMonitor(window);
    if (monitor == nullptr)
        monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr)
        return 1.0;
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);
    int mm_w = 0, mm_h = 0;
    glfwGetMonitorPhysicalSize(monitor, &mm_w, &mm_h);
    double aspect = 1.0;
    if (mode != nullptr && mode->width > 0 && mode->height > 0 && mm_w > 0 && mm_h > 0) {
        aspect = ((double)mm_w / mode->width) / ((double)mm_h / mode->height);
        // EDID physical sizes are often rough (or fictional on TVs):
        // treat near-square as square, and the absurd as unknown
        if (std::abs(aspect - 1.0) < 0.02 || aspect < 0.5 || aspect > 2.0)
            aspect = 1.0;
    }
    static GLFWmonitor *logged_monitor = nullptr;
    static double logged_aspect = 0;
    if (monitor != logged_monitor || aspect != logged_aspect) {
        logged_monitor = monitor;
        logged_aspect = aspect;
        log.info(eApplication | eVideo,
                 std::format("Display: {} {}x{} px, {}x{} mm, pixel aspect {:.3f}", glfwGetMonitorName(monitor),
                             mode != nullptr ? mode->width : 0, mode != nullptr ? mode->height : 0,
                             mm_w, mm_h, aspect));
    }
    return aspect;
}
