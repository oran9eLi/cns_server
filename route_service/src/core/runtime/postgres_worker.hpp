#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <mutex>
#include <stop_token>

#include "core/queue/bounded_queue.hpp"
#include "core/runtime/device_service.hpp"

namespace cns::runtime {

class PostgresWorker {
 public:
  class StorePort {
   public:
    virtual ~StorePort() = default;
    virtual std::expected<device::DeviceRecord, std::string> Provision(
        const protocol::Registration&, TimePoint) = 0;
    virtual std::expected<void, std::string> Write(
        const persistence::DesiredDeviceWrite&) = 0;
    virtual std::expected<void, std::string> ReconnectAndValidate() = 0;
  };

  using ResultSink = std::function<void(DatabaseResult)>;
  using ReplayRequest = std::function<void()>;
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;

  PostgresWorker(StorePort& store, ResultSink results, ReplayRequest replay,
                 SteadyNow steady_now, std::size_t capacity = 1024);

  void SubmitProvision(protocol::Registration registration, TimePoint at);
  void SubmitWrite(persistence::DesiredDeviceWrite write);
  void Run(std::stop_token stop);
  bool FlushAndStop(std::chrono::seconds timeout);
  void ProcessReady();

 private:
  struct Task {
    std::optional<protocol::Registration> registration;
    TimePoint received_at{};
    std::optional<persistence::DesiredDeviceWrite> write;
  };

  void Notify();
  bool Execute(Task& task);

  StorePort& store_;
  ResultSink results_;
  ReplayRequest replay_;
  SteadyNow steady_now_;
  queue::BoundedQueue<Task> incoming_;
  std::deque<Task> pending_;
  bool unavailable_ = false;
  std::atomic_bool stopping_{false};
  std::chrono::steady_clock::time_point reconnect_at_{};
  mutable std::mutex wake_mutex_;
  std::condition_variable_any wake_;
};

}  // namespace cns::runtime
