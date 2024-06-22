//
// Created by staffanu on 6/22/24.
//

#include "SdShaders.h"

SdShaders::SdShaders(Logger &log, const std::string &executable_dir, musevk::VulkanManager &manager,
                     musevk::CommandPool &command_pool) :
                     m_log(log),
                     m_vulkan_manager(manager)

                     {

}

ResultImages SdShaders::getResultImages() {
    return ResultImages { m_image_out, m_image_Y_out, m_image_V_out, m_image_U_out};
}
