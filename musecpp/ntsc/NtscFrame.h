//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_NTSCFRAME_H
#define MUSECPP_NTSCFRAME_H


#include "util/Logger.h"
#include "musevk/VulkanManager.h"

class NtscFrame {
public:
    NtscFrame(Logger &log, int frame_no, musevk::VulkanManager &manager);
};


#endif //MUSECPP_NTSCFRAME_H
