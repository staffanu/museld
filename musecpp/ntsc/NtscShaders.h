//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_NTSCSHADERS_H
#define MUSECPP_NTSCSHADERS_H

#include <string>
#include "musevk/VulkanManager.h"
#include "musevk/CommandPool.h"
#include "util/Logger.h"
#include "ResultImages.h"

class NtscShaders {
public:
    NtscShaders(Logger &log, std::string const &executable_dir, musevk::VulkanManager &manager, musevk::CommandPool &command_pool);

    NtscShaders(NtscShaders &other) = delete;
    void operator=(const NtscShaders &) = delete;

    ResultImages getResultImages();

private:
    Logger &m_log;
    musevk::VulkanManager &m_vulkan_manager;

    // used for final result
    std::shared_ptr<musevk::VulkanImage> m_image_out;
    std::shared_ptr<musevk::VulkanBuffer> m_image_Y_out; // only used if writing to file using ffmpeg
    std::shared_ptr<musevk::VulkanBuffer> m_image_U_out; // ..
    std::shared_ptr<musevk::VulkanBuffer> m_image_V_out; // ..
};


#endif //MUSECPP_NTSCSHADERS_H
