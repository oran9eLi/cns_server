#include "core/runtime/postgres_worker.hpp"

#include <thread>
#include <utility>

namespace cns::runtime {
namespace { constexpr auto kReconnectDelay = std::chrono::seconds{5}; }

PostgresWorker::PostgresWorker(StorePort& store, ResultSink results,
    ReplayRequest replay, SteadyNow steady_now, std::size_t capacity)
    : store_(store), results_(std::move(results)), replay_(std::move(replay)),
      steady_now_(std::move(steady_now)), incoming_(capacity) {}

void PostgresWorker::SubmitProvision(protocol::Registration registration,
                                     TimePoint at) {
  if (incoming_.TryPush(Task{std::move(registration), at, std::nullopt})) Notify();
}
void PostgresWorker::SubmitWrite(persistence::DesiredDeviceWrite write) {
  if (incoming_.TryPush(Task{std::nullopt, {}, std::move(write)})) Notify();
}

void PostgresWorker::Run(std::stop_token stop) {
  std::stop_callback callback(stop, [this] { Notify(); });
  while (!stop.stop_requested() && !stopping_) {
    ProcessReady();
    std::unique_lock lock(wake_mutex_);
    wake_.wait_for(lock, stop, std::chrono::milliseconds{100}, [this] {
      return stopping_ || incoming_.Size() != 0;
    });
  }
}

bool PostgresWorker::FlushAndStop(std::chrono::seconds timeout) {
  stopping_ = true;
  incoming_.Close();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    ProcessReady();
    if (incoming_.Size() == 0 && pending_.empty()) return true;
    if (unavailable_) return false;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void PostgresWorker::ProcessReady() {
  while (incoming_.Size() != 0) {
    if (auto task = incoming_.WaitPop()) pending_.push_back(std::move(*task));
  }
  const auto now = steady_now_();
  if (unavailable_) {
    if (now < reconnect_at_) return;
    auto reconnected = store_.ReconnectAndValidate();
    if (!reconnected) {
      reconnect_at_ = now + kReconnectDelay;
      return;
    }
    unavailable_ = false;
    results_({DatabaseResult::Kind::kRecovered, {}, 0, std::nullopt, {}});
    replay_();
  }
  while (!pending_.empty()) {
    if (!Execute(pending_.front())) {
      // 未建档设备由业务线程在断线时拒绝；恢复后只接受 retained replay，
      // 避免用故障前的旧候选偷偷建档。已登记设备写入则必须保留补写。
      if (pending_.front().registration) pending_.pop_front();
      return;
    }
    pending_.pop_front();
  }
}

bool PostgresWorker::Execute(Task& task) {
  if (task.registration) {
    auto result = store_.Provision(*task.registration, task.received_at);
    if (result) {
      results_({DatabaseResult::Kind::kProvisioned, task.registration->vendor_id,
                result->revision, std::move(*result), {}});
      return true;
    }
    unavailable_ = true;
    reconnect_at_ = steady_now_() + kReconnectDelay;
    results_({DatabaseResult::Kind::kUnavailable, task.registration->vendor_id,
              0, std::nullopt, result.error()});
    return false;
  }
  auto result = store_.Write(*task.write);
  if (result) {
    results_({DatabaseResult::Kind::kWriteCompleted, task.write->record.vendor_id,
              task.write->revision, std::nullopt, {}});
    return true;
  }
  unavailable_ = true;
  reconnect_at_ = steady_now_() + kReconnectDelay;
  results_({DatabaseResult::Kind::kUnavailable, task.write->record.vendor_id,
            task.write->revision, std::nullopt, result.error()});
  return false;
}

void PostgresWorker::Notify() { wake_.notify_all(); }
}  // namespace cns::runtime
