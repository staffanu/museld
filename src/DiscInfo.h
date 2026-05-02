//
// Created by staffanu on 6/22/24.
//

#ifndef MUSECPP_DISCINFO_H
#define MUSECPP_DISCINFO_H

#include <vector>
#include <string>

class DiscInfo {
public:
    [[nodiscard]] virtual std::vector<std::string> asStrings() const = 0;

protected:
    DiscInfo() = default;
};

#endif //MUSECPP_DISCINFO_H
