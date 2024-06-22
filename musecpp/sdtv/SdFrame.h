//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_SDFRAME_H
#define MUSECPP_SDFRAME_H


#include "util/Logger.h"
#include "musevk/VulkanManager.h"

class SdFrame {
public:
    SdFrame(Logger &log, int frame_no, musevk::VulkanManager &manager);
};


#endif //MUSECPP_SDFRAME_H
