//
// Created by staffanu on 2/22/24.
//

#ifndef MUSECPP_SIZE_H
#define MUSECPP_SIZE_H

#include <cstdint>

namespace musevk {
    // Represents a size in three dimensions used for workgroup size and buffer size.
    struct Size {
        Size(uint32_t x, uint32_t y = 1, uint32_t z = 1)
                : x_size(x),
                  y_size(y),
                  z_size(z) {};
        uint32_t x_size;
        uint32_t y_size;
        uint32_t z_size;

        [[nodiscard]] uint32_t numberOfElements() const {
            return x_size * y_size * z_size;
        }
    };
}

#endif //MUSECPP_SIZE_H
