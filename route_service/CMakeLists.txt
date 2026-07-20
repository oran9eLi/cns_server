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

if(NOT MOSQUITTO_VERSION VERSION_EQUAL "2.0.22")
  message(FATAL_ERROR
    "本工程要求系统 libmosquitto 2.0.22，当前版本为 ${MOSQUITTO_VERSION}"
  )
endif()

if(NOT PQXX_VERSION MATCHES "^7\\.10\\.[0-9]+$")
  message(FATAL_ERROR "本工程要求系统 libpqxx 7.10.x，当前版本为 ${PQXX_VERSION}")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "libpqxx 7.10 兼容层只支持 Linux GNU 工具链")
endif()
find_program(CNS_READELF NAMES readelf)
if(NOT CNS_READELF)
  message(FATAL_ERROR "libpqxx 7.10 兼容层要求安装 binutils（缺少 readelf）")
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
  COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX_COMPILER_ARG1}
          -fuse-ld=bfd -Wl,--version
  RESULT_VARIABLE CNS_CXX_LINKER_PROBE_RESULT
  OUTPUT_VARIABLE CNS_CXX_LINKER_PROBE_OUTPUT
  ERROR_VARIABLE CNS_CXX_LINKER_PROBE_ERROR
)
string(STRIP "${CNS_CXX_LINKER_PROBE_OUTPUT}" CNS_CXX_LINKER_PROBE_OUTPUT)
string(STRIP "${CNS_CXX_LINKER_PROBE_ERROR}" CNS_CXX_LINKER_PROBE_ERROR)
if(NOT CNS_CXX_LINKER_PROBE_RESULT EQUAL 0 OR
   NOT CNS_CXX_LINKER_PROBE_OUTPUT MATCHES "GNU ld")
  message(FATAL_ERROR
    "C++ 编译驱动无法选择支持 version script 的 GNU ld。\n"
    "stdout：${CNS_CXX_LINKER_PROBE_OUTPUT}\n"
    "stderr：${CNS_CXX_LINKER_PROBE_ERROR}"
  )
endif()

# libpqxx 7.10 在新 GCC/glibc 上会因 type_name 模板变量抢占而重复析构。
# 所有消费者必须通过本目标链接，选择性本地化消费端实例。
add_library(pqxx_compat INTERFACE)
target_link_libraries(pqxx_compat INTERFACE PkgConfig::PQXX)
target_link_options(pqxx_compat INTERFACE
  -fuse-ld=bfd
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/pqxx-7-type-name.map"
)

enable_testing()

add_library(route_core STATIC
  src/core/cli/command_line.cpp
  src/core/command/command_router.cpp
  src/core/command/command_state.cpp
  src/core/command/source_request.cpp
  src/core/command/config_policy.cpp
  src/core/config/app_config.cpp
  src/core/device/device_registry.cpp
  src/core/logging/logger.cpp
  src/core/mqtt_topic/device_topic.cpp
  src/core/protocol/device_message.cpp
  src/core/protocol/time_format.cpp
  src/core/state_event/state_event.cpp
  src/core/migration/migration.cpp
  src/core/migration/migration_plan.cpp
  src/core/persistence/dirty_state.cpp
  src/core/runtime/rate_limiter.cpp
  src/core/runtime/device_ingress.cpp
  src/core/runtime/device_service.cpp
  src/core/runtime/postgres_worker.cpp
  src/core/runtime/service_runner.cpp
)
target_include_directories(route_core PUBLIC src)
target_link_libraries(route_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(route_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(route_adapters STATIC
  src/adapters/mqtt/mqtt_client.cpp
  src/adapters/postgres/postgres_store.cpp
)
target_include_directories(route_adapters PUBLIC src)
target_link_libraries(route_adapters PUBLIC route_core pqxx_compat PkgConfig::MOSQUITTO)
target_compile_options(route_adapters PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(route_service src/main.cpp src/service_environment.cpp)
target_link_libraries(route_service PRIVATE route_adapters)
target_compile_options(route_service PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(route_service PRIVATE
  CNS_ROUTE_SERVICE_VERSION="${PROJECT_VERSION}"
)

add_test(NAME route_service_help COMMAND route_service --help)
set_tests_properties(route_service_help PROPERTIES
  PASS_REGULAR_EXPRESSION "用法："
)

add_test(NAME route_service_version COMMAND route_service --version)
set_tests_properties(route_service_version PROPERTIES
  PASS_REGULAR_EXPRESSION "0\\.1\\.0"
)

add_executable(test_command_line tests/test_command_line.cpp)
target_link_libraries(test_command_line PRIVATE route_core doctest::doctest)
target_compile_options(test_command_line PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME command_line COMMAND test_command_line)

add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE route_core doctest::doctest)
target_compile_options(test_app_config PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME app_config COMMAND test_app_config)

add_executable(test_device_topic tests/test_device_topic.cpp)
target_link_libraries(test_device_topic PRIVATE route_core doctest::doctest)
target_compile_options(test_device_topic PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME device_topic COMMAND test_device_topic)

add_executable(test_device_message tests/test_device_message.cpp)
target_link_libraries(test_device_message PRIVATE route_core doctest::doctest)
target_compile_options(test_device_message PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME device_message COMMAND test_device_message)

add_executable(test_command_protocol tests/test_command_protocol.cpp)
target_link_libraries(test_command_protocol PRIVATE route_core doctest::doctest)
target_compile_options(test_command_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME command_protocol COMMAND test_command_protocol)

add_executable(test_command_router tests/test_command_router.cpp)
target_link_libraries(test_command_router PRIVATE route_core doctest::doctest)
target_compile_options(test_command_router PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME command_router COMMAND test_command_router)

add_executable(test_command_state tests/test_command_state.cpp)
target_link_libraries(test_command_state PRIVATE route_core doctest::doctest)
target_compile_options(test_command_state PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME command_state COMMAND test_command_state)

add_executable(test_device_registry tests/test_device_registry.cpp)
target_link_libraries(test_device_registry PRIVATE route_core doctest::doctest)
target_compile_options(test_device_registry PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME device_registry COMMAND test_device_registry)

add_executable(test_dirty_state tests/test_dirty_state.cpp)
target_link_libraries(test_dirty_state PRIVATE route_core doctest::doctest)
target_compile_options(test_dirty_state PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME dirty_state COMMAND test_dirty_state)

add_executable(test_device_ingress tests/test_device_ingress.cpp)
target_link_libraries(test_device_ingress PRIVATE route_core doctest::doctest)
target_compile_options(test_device_ingress PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME device_ingress COMMAND test_device_ingress)

add_executable(test_state_event tests/test_state_event.cpp)
target_link_libraries(test_state_event PRIVATE route_core doctest::doctest)
target_compile_options(test_state_event PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME state_event COMMAND test_state_event)

add_executable(test_logger tests/test_logger.cpp)
target_link_libraries(test_logger PRIVATE route_core doctest::doctest)
target_compile_options(test_logger PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME logger COMMAND test_logger)

add_executable(test_bounded_queue tests/test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE route_core doctest::doctest)
target_compile_options(test_bounded_queue PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME bounded_queue COMMAND test_bounded_queue)
set_tests_properties(bounded_queue PROPERTIES TIMEOUT 10)

add_executable(test_rate_limiter tests/test_rate_limiter.cpp)
target_link_libraries(test_rate_limiter PRIVATE route_adapters doctest::doctest)
target_compile_options(test_rate_limiter PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(test_rate_limiter PRIVATE
  LINKER:--wrap=mosquitto_connect_async
  LINKER:--wrap=mosquitto_loop_start
  LINKER:--wrap=mosquitto_disconnect
  LINKER:--wrap=mosquitto_loop_stop
  LINKER:--wrap=mosquitto_destroy
  LINKER:--wrap=mosquitto_lib_cleanup
  LINKER:--wrap=mosquitto_new
  LINKER:--wrap=mosquitto_connect_callback_set
  LINKER:--wrap=mosquitto_disconnect_callback_set
  LINKER:--wrap=mosquitto_log_callback_set
  LINKER:--wrap=cns_runtime_wait_interruptibly
)

add_test(NAME rate_limiter COMMAND test_rate_limiter)

add_executable(test_mqtt_business tests/test_mqtt_business.cpp)
target_link_libraries(test_mqtt_business PRIVATE route_adapters doctest::doctest)
target_compile_options(test_mqtt_business PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_mqtt_business PRIVATE
  CNS_MQTT_CLIENT_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/mqtt/mqtt_client.cpp"
)
target_link_options(test_mqtt_business PRIVATE
  LINKER:--wrap=mosquitto_new
  LINKER:--wrap=mosquitto_connect_callback_set
  LINKER:--wrap=mosquitto_disconnect_callback_set
  LINKER:--wrap=mosquitto_message_callback_set
  LINKER:--wrap=mosquitto_subscribe
  LINKER:--wrap=mosquitto_unsubscribe
  LINKER:--wrap=mosquitto_publish
  LINKER:--wrap=mosquitto_connect_async
  LINKER:--wrap=mosquitto_loop_start
  LINKER:--wrap=mosquitto_disconnect
  LINKER:--wrap=mosquitto_loop_stop
  LINKER:--wrap=cns_runtime_wait_interruptibly
)
add_test(NAME mqtt_business COMMAND test_mqtt_business)

add_executable(test_shutdown_flag tests/test_shutdown_flag.cpp)
target_link_libraries(test_shutdown_flag PRIVATE route_core doctest::doctest)
target_compile_options(test_shutdown_flag PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME shutdown_flag COMMAND test_shutdown_flag)

add_executable(test_service_runner tests/test_service_runner.cpp)
target_link_libraries(test_service_runner PRIVATE route_core doctest::doctest)
target_compile_options(test_service_runner PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME service_runner COMMAND test_service_runner)

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
  CNS_POSTGRES_STORE_HEADER_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/postgres/postgres_store.hpp"
)

add_test(NAME postgres_store COMMAND test_postgres_store)

add_executable(test_postgres_device_store tests/test_postgres_device_store.cpp)
target_link_libraries(test_postgres_device_store PRIVATE route_adapters doctest::doctest)
target_compile_options(test_postgres_device_store PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_postgres_device_store PRIVATE
  CNS_POSTGRES_STORE_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/postgres/postgres_store.cpp"
)

add_test(NAME postgres_device_store COMMAND test_postgres_device_store)

add_executable(test_pqxx_compat tests/test_pqxx_compat.cpp)
target_link_libraries(test_pqxx_compat PRIVATE pqxx_compat)
target_compile_options(test_pqxx_compat PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME pqxx_compat COMMAND test_pqxx_compat)

add_executable(test_device_service tests/test_device_service.cpp)
target_link_libraries(test_device_service PRIVATE route_core doctest::doctest)
target_compile_options(test_device_service PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME device_service COMMAND test_device_service)
set_tests_properties(device_service PROPERTIES TIMEOUT 10)

add_test(NAME pqxx_elf_symbols
  COMMAND ${CMAKE_COMMAND}
    -DREADELF=${CNS_READELF}
    -DPQXX_COMPAT_BINARY=$<TARGET_FILE:test_pqxx_compat>
    -DPOSTGRES_STORE_BINARY=$<TARGET_FILE:test_postgres_store>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pqxx_elf.cmake
)
