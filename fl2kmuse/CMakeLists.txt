cmake_minimum_required(VERSION 3.28)
project(fl2kmuse)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)

pkg_check_modules(FL2K REQUIRED libosmo-fl2k)

add_executable(fl2kmuse
        main.cpp
        MuseSignalGenerator.cpp
        MuseSignalGenerator.h
)

target_link_libraries(fl2kmuse
        PRIVATE ${FL2K_LIBRARIES}
)
