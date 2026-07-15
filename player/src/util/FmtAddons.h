// Copyright 2023-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

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
