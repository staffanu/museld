//
// Created by staffanu on 4/17/23.
//

#ifndef MUSECPP_MUSETYPES_H
#define MUSECPP_MUSETYPES_H

#include <fmt/format.h>
#include <optional>

// The input file has 10 bit resolution in the range 0...1023.
// This constant is the relation to the values specified in the MUSE documents.
#define MUSE_SHORT_INPUT_MULT 4

#define MUSE_TOTAL_HEIGHT 1125
#define MUSE_TOTAL_WIDTH 480
#define MUSE_BUF_HEIGHT 516
#define MUSE_Y_BUF_WIDTH 374
#define MUSE_C_BUF_WIDTH 94

namespace fmt {
    template <typename T>
    struct formatter<std::optional<T>>:fmt::formatter<T> {
        template <typename FormatContext>
        auto format(const std::optional<T>& opt, FormatContext& ctx) {
            if (opt) {
                fmt::formatter<T>::format(*opt, ctx);
                return ctx.out();
            }
            return fmt::format_to(ctx.out(), "nullopt");
        }
    };
}

#endif //MUSECPP_MUSETYPES_H
