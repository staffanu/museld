//
// Created by staffanu on 2/25/24.
//

#ifndef MUSECPP_VULKANUTIL_H
#define MUSECPP_VULKANUTIL_H

#include <vector>

namespace musevk {
    class VulkanUtil {
    public:
        static std::vector<uint32_t> loadSpirv(std::string const &executable_dir, std::string const &filename);


    };
}


#endif //MUSECPP_VULKANUTIL_H
