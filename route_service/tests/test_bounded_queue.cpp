// 本文件负责验证线程安全有界队列的容量、关闭、阻塞与并发语义。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/queue/bounded_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
  cns::queue::BoundedQueue<int> queue(1);
  auto result = std::async(std::launch::async, [&queue] { return queue.WaitPop(); });

  CHECK(result.wait_for(50ms) == std::future_status::timeout);
  REQUIRE(queue.TryPush(42));
  REQUIRE(result.wait_for(1s) == std::future_status::ready);
  CHECK(result.get() == 42);
}

TEST_CASE("关闭唤醒空队列消费者") {
  cns::queue::BoundedQueue<int> queue(1);
  auto result = std::async(std::launch::async, [&queue] { return queue.WaitPop(); });

  CHECK(result.wait_for(50ms) == std::future_status::timeout);
  queue.Close();
  REQUIRE(result.wait_for(1s) == std::future_status::ready);
  CHECK_FALSE(result.get().has_value());
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
