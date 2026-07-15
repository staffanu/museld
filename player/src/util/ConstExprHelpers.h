// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_CONSTEXPRHELPERS_H
#define MUSECPP_CONSTEXPRHELPERS_H

class ConstHelpers {
public:
    static constexpr unsigned log2(size_t x) {
        if (x == 1)
            return 0;
        else {
            assert((x & 1) == 0);
            return log2(x >> 1) + 1;
        }
    }
};

#endif //MUSECPP_CONSTEXPRHELPERS_H
