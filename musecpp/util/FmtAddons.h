//
// Created by staffanu on 6/16/24.
//

#ifndef MUSECPP_FMTADDONS_H
#define MUSECPP_FMTADDONS_H

#include <fmt/format.h>
#include <optional>

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

#endif //MUSECPP_FMTADDONS_H
