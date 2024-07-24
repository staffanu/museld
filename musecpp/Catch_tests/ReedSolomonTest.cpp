//
// Created by staffanu on 2/10/24.
//
#include <catch2/catch_all.hpp>
#include <algorithm>
#include <iostream>
#include "efm/ReedSolomon.h"
#include "util/Logger.h"

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

        ReedSolomon<0x11d, 2> rs(32, 28, 0, true, true);

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
        ReedSolomon<0x11d, 2> rs(32, 28, 0, true, true);

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
        ReedSolomon<0x11d, 2> rs(32, 28, 0, true, true);

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

// This test just checks that the decoder doesn't crash on the provided example -- it is not a correctable error
TEST_CASE("Correct 1 error, 2 erasures problem example") {
    ReedSolomon<0x11d, 2> rs(32, 28, 0, true, true);

    std::vector<ByteWithErasureFlag> data{
//            {0x53, false}, {0xb9, false}, {0x41, false}, {0x24, false},
//            {0x61, false}, {0x2,  false}, {0xfb, false}, {0x23, false},
//            {0x66, false}, {0xff, true}, {0x3c, false}, {0xfe, false},
//            {0xb3, false}, {0x2,  false}, {0xcd, false}, {0x0,  false},
//            {0x67, false}, {0x8f, false}, {0x79, false}, {0xa8, false},
//            {0x11, false}, {0xff, true}, {0x98, false}, {0x3,  false},
//            {0xa5, false}, {0xfc, false}, {0xcf, false}, {0x1,  false},
//            {0xe2, false}, {0xff, false}, {0xf3, false}, {0x1,  false},

            {0x49, false}, {0x6a, false}, {0xee, false}, {0x63, false},
            {0xff, true}, {0xfc, false}, {0xc8, false}, {0x0, false},
            {0x9e, false}, {0xff, false}, {0xe1, false}, {0x0, false},
            {0x63, false}, {0xfc, false}, {0xba, false}, {0x0, false},
            {0x66, false}, {0x92, false}, {0x2b, false}, {0xee, false},
            {0x54, false}, {0x3, false}, {0x76, false}, {0xff, true},
            {0xff, true}, {0xff, false}, {0x9e, false}, {0xff, false},
            {0x42, false}, {0xff, false}, {0x7b, false}, {0x3, false},
    };
    std::reverse(data.begin(), data.end());

    printData(data);
    rs.decode(data);
    printData(data);

    Logger log(Logger::c_log_all);
    rs.printStatistics(log, "");

//    REQUIRE(std::all_of(video_data.cbegin(), video_data.cend(), [](ByteWithErasureFlag b) -> bool {
//        return b.byteValue() == 0 && !b.isErased();
//    }));
}
