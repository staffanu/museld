//
// Created by staffanu on 2/10/24.
//
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <algorithm>
#include "efm/ReedSolomon.h"
#include "Logger.h"

void printData(std::vector<ByteWithErasureFlag> const &data) {
    for (int i = 0; i < 32; i++) {
        std::cout << (int) data[i].byteValue();
        if (data[i].isErased())
            std::cout << "*";
        std::cout << " ";
    }
    std::cout << std::endl;
}

TEST_CASE("Reed solomon corrects errors") {
    for (int no_errors = 1; no_errors <= 2; no_errors++) {

        ReedSolomon<0x11d, 2> rs(32, 28, 0, true);

        std::vector<ByteWithErasureFlag> data;
        data.resize(32);
        for (int i = 0; i < no_errors; i++)
            data[11 + i * 5] = ByteWithErasureFlag(3 + i * 23);

        rs.decode(data);

        REQUIRE(std::all_of(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool {
            return b.byteValue() == 0 && !b.isErased();
        }));
    }
}

TEST_CASE("Reed solomon corrects erasures") {
    for (int no_erasures = 1; no_erasures <= 4; no_erasures++) {
        ReedSolomon<0x11d, 2> rs(32, 28, 0, true);

        std::vector<ByteWithErasureFlag> data;
        data.resize(32);
        for (int i = 0; i < no_erasures; i++)
            data[7 + i * 5] = ByteWithErasureFlag(3 + i * 18, true);

        std::cout << "Testing " << no_erasures << " erasures" << std::endl;
        printData(data);
        rs.decode(data);

        REQUIRE(std::all_of(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool {
            return b.byteValue() == 0 && !b.isErased();
        }));
    }
}

TEST_CASE("Reed solomon corrects one error and erasures") {
    for (int no_erasures = 1; no_erasures <= 2; no_erasures++) {
        ReedSolomon<0x11d, 2> rs(32, 28, 0, true);

        std::vector<ByteWithErasureFlag> data;
        data.resize(32);
        data[30] = ByteWithErasureFlag(251); // error
        for (int i = 0; i < no_erasures; i++)
            data[7 + i * 5] = ByteWithErasureFlag(3 + i * 18, true);

        std::cout << "Testing one error, " << no_erasures << " erasures" << std::endl;
        printData(data);
        rs.decode(data);

        Logger log(Logger::c_log_all);
        rs.printStatistics(log, "");

        REQUIRE(std::all_of(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool {
            return b.byteValue() == 0 && !b.isErased();
        }));
    }
}
