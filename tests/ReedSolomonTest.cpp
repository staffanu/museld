//
// Created by staffanu on 2/10/24.
//
#include <catch2/catch_all.hpp>
#include <algorithm>
#include <iostream>
#include "rs/ReedSolomon.h"

void printData(std::vector<ByteWithErasureFlag> const &data) {
    for (int i = 0; i < data.size(); i++) {
        std::cout << (int) data[i].byteValue();
        if (data[i].isErased())
            std::cout << "*";
        std::cout << " ";
    }
    std::cout << std::endl;
}

TEST_CASE("Reed solomon corrects errors") {
    for (int no_errors = 1; no_errors <= 2; no_errors++) {

        ReedSolomon<0x11d, 2> rs(32, 28, 0, RS_MAX, true);

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

// RS_C2 supports up to 3 erasures with no errors; 4-erasure correction requires RS_MAX with erasure path enabled
TEST_CASE("Reed solomon corrects erasures") {
    for (int no_erasures = 1; no_erasures <= 3; no_erasures++) {
        ReedSolomon<0x11d, 2> rs(32, 28, 0, RS_C2, true);

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

// RS_C2 can correct 1 error + 1 erasure (treats erasure as unknown error location)
TEST_CASE("Reed solomon corrects one error and one erasure") {
    ReedSolomon<0x11d, 2> rs(32, 28, 0, RS_C2, true);

    std::vector<ByteWithErasureFlag> data;
    data.resize(32);
    data[30] = ByteWithErasureFlag(251); // error
    data[7] = ByteWithErasureFlag(3, true); // erasure

    std::cout << "Testing one error, one erasure" << std::endl;
    printData(data);
    rs.decode(data);

    std::cout << rs.statistics() << std::endl;

    REQUIRE(std::all_of(data.cbegin(), data.cend(), [](ByteWithErasureFlag b) -> bool {
        return b.byteValue() == 0 && !b.isErased();
    }));
}

// This test just checks that the decoder doesn't crash on the provided example -- it is not a correctable error
TEST_CASE("Correct 1 error, 2 erasures problem example") {
    ReedSolomon<0x11d, 2> rs(32, 28, 0, RS_MAX, true);

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

    std::cout << rs.statistics() << std::endl;

//    REQUIRE(std::all_of(video_data.cbegin(), video_data.cend(), [](ByteWithErasureFlag b) -> bool {
//        return b.byteValue() == 0 && !b.isErased();
//    }));
}
