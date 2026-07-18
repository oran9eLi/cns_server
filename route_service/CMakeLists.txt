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

if(NOT PQXX_VERSION MATCHES "^7\\.10\\.[0-9]+$")
  message(FATAL_ERROR "本工程要求系统 libpqxx 7.10.x，当前版本为 ${PQXX_VERSION}")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "libpqxx 7.10 兼容层只支持 Linux GNU 工具链")
endif()
set(CNS_PQXX_SHARED FALSE)
foreach(PQXX_LIBRARY IN LISTS PQXX_LINK_LIBRARIES)
  if(PQXX_LIBRARY MATCHES "\\.so($|\\.)")
    set(CNS_PQXX_SHARED TRUE)
  endif()
endforeach()
if(NOT CNS_PQXX_SHARED)
  message(FATAL_ERROR "libpqxx 7.10 兼容层要求 pkg-config 提供共享库")
endif()
execute_process(
  COMMAND "${CMAKE_LINKER}" --version
  RESULT_VARIABLE CNS_LINKER_VERSION_RESULT
  OUTPUT_VARIABLE CNS_LINKER_VERSION_OUTPUT
  ERROR_VARIABLE CNS_LINKER_VERSION_ERROR
)
if(NOT CNS_LINKER_VERSION_RESULT EQUAL 0 OR
   NOT CNS_LINKER_VERSION_OUTPUT MATCHES "GNU ld")
  message(FATAL_ERROR "libpqxx 7.10 兼容层要求 GNU ld version script 支持")
endif()

# libpqxx 7.10 在新 GCC/glibc 上会因 type_name 模板变量抢占而重复析构。
# 所有消费者必须通过本目标链接，选择性本地化消费端实例。
add_library(pqxx_compat INTERFACE)
target_link_libraries(pqxx_compat INTERFACE PkgConfig::PQXX)
target_link_options(pqxx_compat INTERFACE
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/pqxx-7-type-name.map"
)

enable_testing()

add_library(route_core STATIC
  src/core/cli/command_line.cpp
  src/core/config/app_config.cpp
  src/core/logging/logger.cpp
  src/core/migration/migration.cpp
  src/core/migration/migration_plan.cpp
)
target_include_directories(route_core PUBLIC src)
target_link_libraries(route_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(route_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(route_adapters STATIC
  src/adapters/postgres/postgres_store.cpp
)
target_include_directories(route_adapters PUBLIC src)
target_link_libraries(route_adapters PUBLIC route_core pqxx_compat)
target_compile_options(route_adapters PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(test_command_line tests/test_command_line.cpp)
target_link_libraries(test_command_line PRIVATE route_core doctest::doctest)
target_compile_options(test_command_line PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME command_line COMMAND test_command_line)

add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE route_core doctest::doctest)
target_compile_options(test_app_config PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME app_config COMMAND test_app_config)

add_executable(test_logger tests/test_logger.cpp)
target_link_libraries(test_logger PRIVATE route_core doctest::doctest)
target_compile_options(test_logger PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME logger COMMAND test_logger)

add_executable(test_bounded_queue tests/test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE route_core doctest::doctest)
target_compile_options(test_bounded_queue PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME bounded_queue COMMAND test_bounded_queue)
set_tests_properties(bounded_queue PROPERTIES TIMEOUT 10)

add_executable(test_migration tests/test_migration.cpp)
target_link_libraries(test_migration PRIVATE route_core doctest::doctest)
target_compile_options(test_migration PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME migration COMMAND test_migration)

add_executable(test_migration_plan tests/test_migration_plan.cpp)
target_link_libraries(test_migration_plan PRIVATE route_core doctest::doctest)
target_compile_options(test_migration_plan PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME migration_plan COMMAND test_migration_plan)

add_executable(test_schema_sql tests/test_schema_sql.cpp)
target_link_libraries(test_schema_sql PRIVATE doctest::doctest)
target_compile_options(test_schema_sql PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_schema_sql PRIVATE
  CNS_MIGRATIONS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/migrations"
)

add_test(NAME schema_sql COMMAND test_schema_sql)

add_executable(test_postgres_store tests/test_postgres_store.cpp)
target_link_libraries(test_postgres_store PRIVATE route_adapters doctest::doctest)
target_compile_options(test_postgres_store PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_postgres_store PRIVATE
  CNS_POSTGRES_STORE_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/postgres/postgres_store.cpp"
)

add_test(NAME postgres_store COMMAND test_postgres_store)

add_executable(test_pqxx_compat tests/test_pqxx_compat.cpp)
target_link_libraries(test_pqxx_compat PRIVATE pqxx_compat)
target_compile_options(test_pqxx_compat PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME pqxx_compat COMMAND test_pqxx_compat)
