// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <catch2/catch_all.hpp>
#include <algorithm>
#include <iostream>
#include "bch/BchDecoder.h"

void printData(std::vector<int> const &data) {
    for (int i : data) {
        std::cout << i << " ";
    }
    std::cout << std::endl;
}

TEST_CASE("BCH corrects errors") {
    for (int no_errors = 1; no_errors <= 2; no_errors++) {

        BchDecoder decoder(82, 137, true);

        std::vector<int> data(82);
        for (int i = 0; i < no_errors; i++)
            data[11 + i * 5] = 1;

        decoder.decode(data.data());
//        printData(data);
//        Logger log(Logger::c_log_all);
//        decoder.printStatistics(log, "");

        REQUIRE(std::all_of(data.cbegin(), data.cend(), [](int b) -> bool { return b == 0; }));
    }
}
