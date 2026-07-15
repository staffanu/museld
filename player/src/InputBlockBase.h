// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_INPUTBLOCKBASE_H
#define MUSECPP_INPUTBLOCKBASE_H

#include <vector>

class InputBlockBase {
public:
    std::vector<float> efm_data;

    virtual void writeToFile(int fd, void *buffer) = 0;

protected:
    InputBlockBase() = default;
};

template<class B>
class InputBlockFactory {
public:
//    static B makeBlock();
};

#endif //MUSECPP_INPUTBLOCKBASE_H
