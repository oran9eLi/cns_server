#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/runtime/device_service.hpp"

namespace cns::runtime {

struct DatabaseError {
  enum class Kind { kUnavailable, kPermanent };
  Kind kind;
  std::string message;
};

class PostgresWorker {
 public:
  class StorePort {
   public:
    virtual ~StorePort() = default;
    virtual std::expected<device::DeviceRecord, DatabaseError> Provision(
        const protocol::Registration&, TimePoint) = 0;
    virtual std::expected<void, DatabaseError> Write(
        const persistence::DesiredDeviceWrite&) = 0;
    virtual std::expected<void, DatabaseError> ReconnectAndValidate() = 0;
  };

  using ResultSink = std::function<void(DatabaseResult)>;
  using ReplayRequest = std::function<void()>;
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
  using DiagnosticSink = std::function<void(std::string)>;

  PostgresWorker(StorePort& store, ResultSink results, ReplayRequest replay,
                 SteadyNow steady_now, DiagnosticSink diagnostic = {});

  // 接纳尚未在途的 vendor；关闭、数据库不可用或同 vendor 已在途时返回 false。
  // latest registration 只由 DeviceService 的 pending 状态合并和持有。
  bool SubmitProvision(protocol::Registration registration, TimePoint at);
  bool SubmitWrite(persistence::DesiredDeviceWrite write);
  // 唯一允许调用 StorePort 的入口。
  void Run(std::stop_token stop);
  // 只请求 Run 线程排空并等待，不调用 StorePort。
  bool FlushAndStop(std::chrono::milliseconds timeout);

 private:
  struct ProvisionTask {
    protocol::Registration registration;
    TimePoint received_at;
  };

  void Emit(DatabaseResult result) noexcept;
  void Diagnose(std::string message) noexcept;
  void Finish() noexcept;
  void MergeWrite(persistence::DesiredDeviceWrite write);
  bool HasWork() const;

  StorePort& store_;
  ResultSink results_;
  ReplayRequest replay_;
  SteadyNow steady_now_;
  DiagnosticSink diagnostic_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::unordered_map<std::string, ProvisionTask> provisions_;
  std::unordered_map<std::string, persistence::DesiredDeviceWrite> writes_;
  std::unordered_set<std::string> provisioning_;
  bool unavailable_ = false;
  bool accepting_ = true;
  bool drain_requested_ = false;
  bool worker_stopped_ = false;
  std::chrono::steady_clock::time_point reconnect_at_{};
};

}  // namespace cns::runtime
