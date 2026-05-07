#include "FrameBlitter.h"

#include "PlayerState.h"
#include "ResultImages.h"
#include "musevk/CommandBuffer.h"
#include "musevk/VulkanImage.h"
#include "musevk/VulkanManager.h"

void FrameBlitter::present(musevk::CommandBuffer &command_buffer,
                           ResultImages &images,
                           const PlayerState &state,
                           Decoder::SourceDimensions src,
                           vk::Image swap_chain_image,
                           vk::Extent2D swap_extent,
                           musevk::VulkanManager & /*manager*/,
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

    const double zoom = state.zoom_factor;
    vk::ImageBlit region;
    region.srcOffsets[0] = vk::Offset3D((int32_t)((state.zoom_center.first - 0.5 / zoom) * src.width),
                                        (int32_t)((state.zoom_center.second - 0.5 / zoom) * src.height), 0);
    region.srcOffsets[1] = vk::Offset3D((int32_t)((state.zoom_center.first + 0.5 / zoom) * src.width),
                                        (int32_t)((state.zoom_center.second + 0.5 / zoom) * src.height), 1);
    region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
    region.dstOffsets[1] = vk::Offset3D((int)swap_extent.width, (int)swap_extent.height, 1);
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
