#pragma once
// A small fixed-size worker pool.
//
// v1 spawned one detached thread per peer per write for eager broadcast, and
// said so in a comment: "acceptable at MVP/demo write rates". v2 targets 50
// nodes on one LAN, where that becomes 50 detached threads per write and a
// thread-creation storm under any real write burst. This is the bounded
// upgrade that comment named.
//
// Deliberately minimal: fixed threads, one unbounded queue, no work
// stealing, no priorities. The one non-obvious property it does provide is
// **drop-on-overflow with a counter**: past `max_queued`, new work is
// discarded and counted rather than queued forever. Eager broadcast is a
// latency optimisation whose correctness backstop is gossip anti-entropy, so
// dropping a push under overload is safe (the write still converges, just
// via the next gossip round) whereas growing the queue without limit is not.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace desentry {

class WorkerPool {
 public:
  WorkerPool(size_t threads, size_t max_queued);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Queues `task`. Returns false when the queue is full and the task was
  // dropped (see the header comment on why dropping is the right behaviour
  // for this pool's callers).
  bool Submit(std::function<void()> task);

  void Stop();

  size_t queued() const;
  uint64_t dropped() const { return dropped_.load(); }
  uint64_t completed() const { return completed_.load(); }
  size_t threads() const { return workers_.size(); }

 private:
  void Run();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  size_t max_queued_;
  bool stopped_ = false;
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> completed_{0};
};

}  // namespace desentry
