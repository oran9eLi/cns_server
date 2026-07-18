// 本文件负责验证线程安全有界队列的容量、关闭、阻塞与并发语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/queue/bounded_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct BlockingObservation {
  bool initially_blocked = false;
  bool action_succeeded = false;
  bool became_ready = false;
  bool has_value = false;
  int value = 0;
};

template <typename Scenario>
std::optional<BlockingObservation> RunIsolatedBlockingScenario(Scenario scenario) {
  int observations[2];
  if (pipe(observations) != 0) {
    throw std::runtime_error("无法创建阻塞测试管道");
  }

  const pid_t child = fork();
  if (child < 0) {
    close(observations[0]);
    close(observations[1]);
    throw std::runtime_error("无法创建阻塞测试子进程");
  }
  if (child == 0) {
    close(observations[0]);
    const BlockingObservation observation = scenario();
    const auto* bytes = reinterpret_cast<const char*>(&observation);
    const ssize_t ignored = write(observations[1], bytes, sizeof(observation));
    static_cast<void>(ignored);
    _exit(0);
  }

  close(observations[1]);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  int status = 0;
  pid_t wait_result = waitpid(child, &status, WNOHANG);
  while (wait_result == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
    wait_result = waitpid(child, &status, WNOHANG);
  }
  if (wait_result == 0) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    close(observations[0]);
    return std::nullopt;
  }

  BlockingObservation observation;
  const ssize_t bytes_read =
      read(observations[0], reinterpret_cast<char*>(&observation), sizeof(observation));
  close(observations[0]);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      bytes_read != static_cast<ssize_t>(sizeof(observation))) {
    return std::nullopt;
  }
  return observation;
}

}  // namespace

TEST_CASE("满队列不阻塞且拒绝新元素") {
  cns::queue::BoundedQueue<int> queue(2);

  CHECK(queue.TryPush(1));
  CHECK(queue.TryPush(2));
  CHECK_FALSE(queue.TryPush(3));
  CHECK(queue.Size() == 2);
}

TEST_CASE("关闭后先按FIFO排空再返回关闭") {
  cns::queue::BoundedQueue<int> queue(2);
  REQUIRE(queue.TryPush(1));
  REQUIRE(queue.TryPush(2));

  queue.Close();

  CHECK(queue.WaitPop() == 1);
  CHECK(queue.WaitPop() == 2);
  CHECK_FALSE(queue.WaitPop().has_value());
}

TEST_CASE("容量零被拒绝") {
  CHECK_THROWS_AS(cns::queue::BoundedQueue<int>(0), std::invalid_argument);
}

TEST_CASE("空队列消费者阻塞直到元素到达") {
  const auto observation = RunIsolatedBlockingScenario([] {
    cns::queue::BoundedQueue<int> queue(1);
    auto result = std::async(std::launch::async, [&queue] { return queue.WaitPop(); });
    BlockingObservation observed;
    observed.initially_blocked = result.wait_for(50ms) == std::future_status::timeout;
    observed.action_succeeded = queue.TryPush(42);
    observed.became_ready = result.wait_for(1s) == std::future_status::ready;
    if (observed.became_ready) {
      const auto value = result.get();
      observed.has_value = value.has_value();
      observed.value = value.value_or(0);
    }
    return observed;
  });

  REQUIRE(observation.has_value());
  CHECK(observation->initially_blocked);
  CHECK(observation->action_succeeded);
  CHECK(observation->became_ready);
  CHECK(observation->has_value);
  CHECK(observation->value == 42);
}

TEST_CASE("关闭唤醒空队列消费者") {
  const auto observation = RunIsolatedBlockingScenario([] {
    cns::queue::BoundedQueue<int> queue(1);
    auto result = std::async(std::launch::async, [&queue] { return queue.WaitPop(); });
    BlockingObservation observed;
    observed.initially_blocked = result.wait_for(50ms) == std::future_status::timeout;
    queue.Close();
    observed.action_succeeded = queue.IsClosed();
    observed.became_ready = result.wait_for(1s) == std::future_status::ready;
    if (observed.became_ready) {
      observed.has_value = result.get().has_value();
    }
    return observed;
  });

  REQUIRE(observation.has_value());
  CHECK(observation->initially_blocked);
  CHECK(observation->action_succeeded);
  CHECK(observation->became_ready);
  CHECK_FALSE(observation->has_value);
}

TEST_CASE("仅移动类型支持入队出队且满队列拒绝新元素") {
  cns::queue::BoundedQueue<std::unique_ptr<int>> queue(1);
  REQUIRE(queue.TryPush(std::make_unique<int>(42)));

  auto rejected = std::make_unique<int>(7);
  CHECK_FALSE(queue.TryPush(std::move(rejected)));
  CHECK(rejected == nullptr);

  auto value = queue.WaitPop();
  REQUIRE(value.has_value());
  REQUIRE(*value != nullptr);
  CHECK(**value == 42);
}

TEST_CASE("关闭后拒绝入队且重复关闭安全") {
  cns::queue::BoundedQueue<int> queue(1);

  queue.Close();
  queue.Close();

  CHECK(queue.IsClosed());
  CHECK_FALSE(queue.TryPush(1));
  CHECK(queue.Size() == 0);
}

TEST_CASE("两生产者两消费者传递全部元素且不重复") {
  constexpr int kItemsPerProducer = 500;
  constexpr int kTotalItems = 2 * kItemsPerProducer;
  cns::queue::BoundedQueue<int> queue(17);
  std::vector<int> consumed;
  consumed.reserve(kTotalItems);
  std::mutex consumed_mutex;
  std::atomic_bool stop_producers = false;

  auto consume = [&] {
    while (const auto value = queue.WaitPop()) {
      std::lock_guard lock(consumed_mutex);
      consumed.push_back(*value);
    }
  };
  std::jthread consumer_one(consume);
  std::jthread consumer_two(consume);

  auto produce = [&queue, &stop_producers](int first) {
    for (int offset = 0; offset < kItemsPerProducer && !stop_producers.load();) {
      if (queue.TryPush(first + offset)) {
        ++offset;
      } else {
        std::this_thread::yield();
      }
    }
  };
  auto producer_one = std::async(std::launch::async, produce, 0);
  auto producer_two = std::async(std::launch::async, produce, kItemsPerProducer);

  const bool producer_one_finished = producer_one.wait_for(2s) == std::future_status::ready;
  const bool producer_two_finished = producer_two.wait_for(2s) == std::future_status::ready;
  const bool producers_finished = producer_one_finished && producer_two_finished;
  stop_producers.store(true);
  queue.Close();
  CHECK(producers_finished);
  producer_one.get();
  producer_two.get();
  consumer_one.join();
  consumer_two.join();

  std::ranges::sort(consumed);
  REQUIRE(consumed.size() == kTotalItems);
  for (int value = 0; value < kTotalItems; ++value) {
    CAPTURE(value);
    CHECK(consumed[static_cast<std::size_t>(value)] == value);
  }
}
