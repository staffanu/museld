//
// Created by staffanu on 6/16/24.
//

#ifndef MUSECPP_INPUTBLOCKBASE_H
#define MUSECPP_INPUTBLOCKBASE_H

class InputBlockBase {
public:
    static constexpr int c_max_efm_data_size = 150000; // a bit above 7350 / 30 * 588;

    int efm_data_size;
    std::array<bool, c_max_efm_data_size> efm_data;

    virtual void writeToFile(int fd, void *buffer) = 0;

protected:
    InputBlockBase()
    : efm_data_size(0),
      efm_data() {}
};

template<class B>
class InputBlockFactory {
public:
//    static B makeBlock();
};

#endif //MUSECPP_INPUTBLOCKBASE_H
