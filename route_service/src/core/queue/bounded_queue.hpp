// 本文件负责提供线程安全、非阻塞入队的通用有界队列。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cns::queue {

/**
 * @brief 支持多生产者与多消费者的线程安全有界队列。
 *
 * 入队操作从不等待容量；出队操作在空队列上等待元素或关闭通知。
 */
template <typename T>
class BoundedQueue {
 public:
  /**
   * @brief 创建具有固定容量的队列。
   * @param capacity 队列可容纳的最大元素数，必须大于零。
   * @throws std::invalid_argument 容量为零时抛出。
   */
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("有界队列容量必须大于零");
    }
  }

  /**
   * @brief 尝试以线程安全方式入队且绝不等待容量。
   * @param value 待移动入队的元素。
   * @return 入队成功返回 true；队列已满或已关闭返回 false。
   */
  bool TryPush(T value) {
    {
      std::lock_guard lock(mutex_);
      if (closed_ || queue_.size() >= capacity_) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    condition_.notify_one();
    return true;
  }

  /**
   * @brief 以线程安全方式等待并取出队首元素。
   * @return 按 FIFO 顺序返回元素；关闭后排空队列，关闭且为空时返回空值。
   */
  std::optional<T> WaitPop() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  /**
   * @brief 以线程安全且幂等的方式关闭队列并唤醒全部等待消费者。
   */
  void Close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    condition_.notify_all();
  }

  /**
   * @brief 获取当前元素数量的线程安全快照。
   * @return 调用时刻持锁读取的元素数量。
   */
  [[nodiscard]] std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  /**
   * @brief 获取关闭状态的线程安全快照。
   * @return 队列已经关闭时返回 true。
   */
  [[nodiscard]] bool IsClosed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<T> queue_;
  bool closed_ = false;
};

}  // namespace cns::queue
