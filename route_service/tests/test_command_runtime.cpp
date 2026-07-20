// 本文件验证设备与命令服务共用业务线程的循环挂接边界。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/runtime/device_service.hpp"

using namespace std::chrono_literals;

TEST_CASE("设备业务循环在同一线程驱动命令周期钩子") {
  cns::device::DeviceRegistry registry;
  REQUIRE(registry.Load({}));
  cns::runtime::DeviceService service(
      registry, [](auto, auto) { return true; }, [](auto) { return true; },
      [](auto) {}, [] { return std::chrono::steady_clock::now(); });
  std::stop_source stop;
  std::atomic_int calls{0};
  const auto caller = std::this_thread::get_id();
  std::thread::id hook_thread;
  std::thread worker([&] {
    service.Run(stop.get_token(), [&](auto) {
      hook_thread = std::this_thread::get_id();
      ++calls;
      stop.request_stop();
    });
  });
  worker.join();
  CHECK(calls.load() >= 1);
  CHECK(hook_thread != caller);
}
