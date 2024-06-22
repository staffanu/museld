//
// Created by staffanu on 6/22/24.
//

#include "SdFrame.h"
#include "musevk/VulkanBuffer.h"
#include "musevk/VulkanManager.h"

SdFrame::SdFrame(Logger &log, int frame_no, musevk::VulkanManager &manager) {

    std::make_unique<musevk::VulkanBuffer>(
            manager, musevk::Size(720, 480), 2 /* sizeof(float16) */,
            vk::BufferUsageFlagBits::eStorageBuffer, musevk::eHostRead);
}
