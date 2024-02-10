//
// Created by staffanu on 2/10/24.
//
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <algorithm>
#include "efm/ReedSolomon.h"

TEST_CASE("Reed solomon corrects one error") {
    ReedSolomon<0x11d, 2> rs(32, 28, 0, true);

    std::vector<ByteWithErasureFlag> data;
    data.resize(32);
    data[7] = ByteWithErasureFlag(5);

    rs.decode(data.data());

    REQUIRE(std::all_of(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool {
        return b.value == 0 && !b.erased;
    }));
}
