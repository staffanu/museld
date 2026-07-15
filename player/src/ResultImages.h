// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_RESULTIMAGES_H
#define MUSECPP_RESULTIMAGES_H

#include <memory>
#include "musevk/VulkanImage.h"
#include "musevk/VulkanBuffer.h"

struct ResultImages {
    std::shared_ptr<musevk::VulkanImage> out_image;
    std::shared_ptr<musevk::VulkanBuffer> out_Y;
    std::shared_ptr<musevk::VulkanBuffer> out_U;
    std::shared_ptr<musevk::VulkanBuffer> out_V;
};

#endif //MUSECPP_RESULTIMAGES_H
