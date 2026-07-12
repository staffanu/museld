// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the Gnu General Public License v3 (see gpl-3.0.txt)

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

        bool operator==(const Size &other) const {
            return x_size == other.x_size && y_size == other.y_size && z_size == other.z_size;
        }

        [[nodiscard]] uint32_t numberOfElements() const {
            return x_size * y_size * z_size;
        }
    };
}

#endif //MUSECPP_SIZE_H
