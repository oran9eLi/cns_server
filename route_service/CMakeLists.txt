# 本文件负责定义路由服务的 C++23 构建目标与命令行单元测试。
cmake_minimum_required(VERSION 3.25)
project(cns_route_service VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
find_package(nlohmann_json REQUIRED)
find_package(doctest REQUIRED)

enable_testing()

add_library(route_core STATIC
  src/core/cli/command_line.cpp
  src/core/config/app_config.cpp
)
target_include_directories(route_core PUBLIC src)
target_link_libraries(route_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(route_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(test_command_line tests/test_command_line.cpp)
target_link_libraries(test_command_line PRIVATE route_core doctest::doctest)
target_compile_options(test_command_line PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME command_line COMMAND test_command_line)

add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE route_core doctest::doctest)
target_compile_options(test_app_config PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME app_config COMMAND test_app_config)
