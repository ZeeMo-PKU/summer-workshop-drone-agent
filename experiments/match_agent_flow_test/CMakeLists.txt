cmake_minimum_required(VERSION 3.16)
project(match_agent_flow_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRONE_SDK REQUIRED IMPORTED_TARGET iking_drone_sdk)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_executable(match_flow src/match_flow.cpp)
target_link_libraries(match_flow PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::OPENCV
    PkgConfig::JSONCPP
    pthread)
target_compile_options(match_flow PRIVATE -Wall -Wextra -Wpedantic)

add_executable(flow_logic_test tests/flow_logic_test.cpp)
target_include_directories(flow_logic_test PRIVATE src)
target_compile_options(flow_logic_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(mission_sequence_test tests/mission_sequence_test.cpp)
target_include_directories(mission_sequence_test PRIVATE src)
target_compile_options(mission_sequence_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(recognize_test tests/test_recognize.cpp)
target_include_directories(recognize_test PRIVATE src)
target_link_libraries(recognize_test PRIVATE PkgConfig::OPENCV)
target_compile_options(recognize_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(gimbal_probe src/gimbal_probe.cpp)
target_link_libraries(gimbal_probe PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::OPENCV
    PkgConfig::JSONCPP
    pthread)
target_compile_options(gimbal_probe PRIVATE -Wall -Wextra -Wpedantic)

add_executable(recovery_return src/recovery_return.cpp)
target_link_libraries(recovery_return PRIVATE
    PkgConfig::DRONE_SDK
    PkgConfig::JSONCPP
    pthread)
target_compile_options(recovery_return PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME flow_logic_test COMMAND flow_logic_test)
add_test(NAME mission_sequence_test COMMAND mission_sequence_test)
