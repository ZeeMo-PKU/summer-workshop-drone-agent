cmake_minimum_required(VERSION 3.16)
project(portable_performance_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRONE_SDK REQUIRED IMPORTED_TARGET iking_drone_sdk)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_executable(portable_performance_test src/portable_performance_test.cpp)
target_link_libraries(portable_performance_test PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::OPENCV
    PkgConfig::JSONCPP
    pthread)
target_compile_options(portable_performance_test PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(gripper_control src/gripper_control.cpp)
target_link_libraries(gripper_control PRIVATE
    PkgConfig::DRONE_SDK
    pthread)
target_compile_options(gripper_control PRIVATE
    -Wall -Wextra -Wpedantic)

add_executable(performance_plan_test tests/performance_plan_test.cpp)
target_include_directories(performance_plan_test PRIVATE src)
target_compile_options(performance_plan_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

enable_testing()
add_test(NAME performance_plan_test COMMAND performance_plan_test)
