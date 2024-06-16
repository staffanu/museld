//
// Created by staffanu on 6/16/24.
//

#ifndef MUSECPP_FMTADDONS_H
#define MUSECPP_FMTADDONS_H

#include <format>
#include <optional>

template <typename T>
struct std::formatter<std::optional<T>> : std::formatter<T> {
    template <typename FormatContext>
    auto format(const std::optional<T>& opt, FormatContext& ctx) const {
        if (opt) {
            std::formatter<T>::format(*opt, ctx);
            return ctx.out();
        } else {
            return std::format_to(ctx.out(), "nullopt");
        }
    }
};

#endif //MUSECPP_FMTADDONS_H
