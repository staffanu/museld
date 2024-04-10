//
// Created by staffanu on 4/8/24.
//

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
