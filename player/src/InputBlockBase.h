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
