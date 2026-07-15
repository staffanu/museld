// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_FRAMEBLITTER_H
#define MUSECPP_FRAMEBLITTER_H

#include <vulkan/vulkan.hpp>

#include "Decoder.h"

struct PlayerState;
struct ResultImages;
namespace musevk { class CommandBuffer; class VulkanManager; }

class FrameBlitter {
public:
    void present(musevk::CommandBuffer &command_buffer,
                 ResultImages &images,
                 const PlayerState &state,
                 Decoder::SourceDimensions src,
                 vk::Image swap_chain_image,
                 vk::Extent2D swap_extent,
                 musevk::VulkanManager &manager,
                 vk::Semaphore image_available_semaphore);
};

#endif //MUSECPP_FRAMEBLITTER_H
