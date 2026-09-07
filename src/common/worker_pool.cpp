#include "desentry/common/worker_pool.h"

#include <algorithm>

namespace desentry {

WorkerPool::WorkerPool(size_t threads, size_t max_queued)
    : max_queued_(max_queued == 0 ? 1024 : max_queued) {
  const size_t count = std::max<size_t>(1, threads);
  workers_.reserve(count);
  for (size_t i = 0; i < count; ++i) workers_.emplace_back(&WorkerPool::Run, this);
}

WorkerPool::~WorkerPool() { Stop(); }

bool WorkerPool::Submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopped_) return false;
    if (queue_.size() >= max_queued_) {
      dropped_.fetch_add(1);
      return false;
    }
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void WorkerPool::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopped_) return;
    stopped_ = true;
    // Queued-but-unstarted work is discarded rather than drained: this pool
    // only ever carries best-effort eager pushes, and blocking shutdown on a
    // backlog of them would make stopping a node take as long as the backlog.
    queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

size_t WorkerPool::queued() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

void WorkerPool::Run() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
      if (stopped_ && queue_.empty()) return;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    // A task that throws must not take the worker down with it: these are
    // network sends against peers that can fail in any number of ways, and
    // losing a worker thread per failure would silently shrink the pool.
    try {
      task();
    } catch (const std::exception&) {
    } catch (...) {
    }
    completed_.fetch_add(1);
  }
}

}  // namespace desentry
