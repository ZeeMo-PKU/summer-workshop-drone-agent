cmake_minimum_required(VERSION 3.16)
project(classmate_manual_trigger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRONE_SDK REQUIRED IMPORTED_TARGET iking_drone_sdk)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(match_manual src/match_manual.cpp)
target_link_libraries(match_manual PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::OPENCV
    PkgConfig::JSONCPP
    pthread)
target_compile_options(match_manual PRIVATE -Wall -Wextra -Wpedantic)

add_executable(camera_preflight src/camera_preflight.cpp)
target_link_libraries(camera_preflight PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::JSONCPP
    pthread)
target_compile_options(camera_preflight PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(
    NAME manual_trigger_contract_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/manual_trigger_contract_test.py)
