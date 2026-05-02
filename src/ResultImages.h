//
// Created by staffanu on 6/22/24.
//

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
