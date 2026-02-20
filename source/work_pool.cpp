// Standard library
#include <algorithm>
#include <utility>
#include <vector>

// Third-party
#include <spdlog/spdlog.h>

// Project headers
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/work_pool.hpp>

namespace gtopt
{

void AdaptiveWorkPool::start()
{
  if (running_.exchange(true)) {
    return;
  }

  try {
    cpu_monitor_.set_interval(3 * scheduler_interval_);
    cpu_monitor_.start();
    scheduler_thread_ =
        std::jthread {[this](const std::stop_token& stoken)
                      {
                        pthread_setname_np(pthread_self(), "WorkPoolScheduler");
                        while (!stoken.stop_requested() && running_) {
                          cleanup_completed_tasks();
                          if (should_schedule_new_task()) {
                            schedule_next_task();
                          }
                          std::this_thread::sleep_for(scheduler_interval_);
                        }
                      }};
    SPDLOG_INFO("AdaptiveWorkPool started with {} max threads", max_threads_);
  } catch (const std::exception& e) {
    running_ = false;
    auto msg = std::format("Failed to start AdaptiveWorkPool: {}", e.what());
    SPDLOG_ERROR(msg);
    throw std::runtime_error(msg);
  }
}

void AdaptiveWorkPool::shutdown()
{
  if (!running_) {
    return;
  }

  running_ = false;
  cv_.notify_all();

  if (scheduler_thread_.joinable()) {
    scheduler_thread_.request_stop();
    scheduler_thread_.join();
  }

  {
    const std::scoped_lock lock(active_mutex_);
    for (auto& task : active_tasks_) {
      task.future.wait();
    }
    active_tasks_.clear();
    tasks_active_.store(0, std::memory_order_relaxed);
  }

  cpu_monitor_.stop();
  SPDLOG_INFO("AdaptiveWorkPool shutdown complete");
}

void AdaptiveWorkPool::cleanup_completed_tasks()
{
  const std::scoped_lock<std::mutex> lock(active_mutex_);
  auto new_end = std::ranges::remove_if(
                     active_tasks_,
                     [this](const auto& task)
                     {
                       if (task.is_ready()) {
                         active_threads_ -= task.requirements.estimated_threads;
                         tasks_completed_++;
                         tasks_active_.fetch_sub(1, std::memory_order_relaxed);
                         return true;
                       }
                       return false;
                     })
                     .begin();
  active_tasks_.erase(new_end, active_tasks_.end());
}

bool AdaptiveWorkPool::should_schedule_new_task() const
{
  std::unique_lock queue_lock(queue_mutex_, std::defer_lock);
  std::unique_lock active_lock(active_mutex_, std::defer_lock);
  std::lock(queue_lock, active_lock);

  if (task_queue_.empty()) {
    return false;
  }

  const auto& next_task = task_queue_.top();
  const auto cpu_load = cpu_monitor_.get_load();
  const auto threads_needed = next_task.requirements().estimated_threads;
  const auto current_threads = active_threads_.load();

  if (current_threads + threads_needed > max_threads_) {
    return false;
  }

  auto threshold = max_cpu_threshold_;
  switch (next_task.requirements().priority) {
    case TaskPriority::Critical:
      threshold = 95.0;
      break;
    case TaskPriority::High:
      threshold = max_cpu_threshold_ + 5.0;
      break;
    default:
      break;
  }

  return cpu_load < threshold;
}

void AdaptiveWorkPool::schedule_next_task()
{
  const std::unique_lock queue_lock(queue_mutex_);

  if (task_queue_.empty()) {
    return;
  }

  Task<void> task =
      std::move(const_cast<Task<void>&>(task_queue_.top()));  // NOLINT

  task_queue_.pop();

  tasks_pending_.fetch_sub(1, std::memory_order_relaxed);

  auto req = task.requirements();
  const auto threads_needed = req.estimated_threads;
  active_threads_.fetch_add(threads_needed, std::memory_order_relaxed);

  try {
    auto future = std::async(
        std::launch::async,
        [ntask = std::move(task)]() mutable noexcept
        {
          try {
            ntask.execute();
          } catch (const std::exception& e) {
            SPDLOG_ERROR("Task execution failed: {}", e.what());
          } catch (...) {
            SPDLOG_ERROR("Task execution failed with unknown exception");
          }
        });

    active_tasks_.push_back(ActiveTask {
        .future = std::move(future),
        .requirements = std::move(req),
        .start_time = std::chrono::steady_clock::now(),
    });
    tasks_active_.fetch_add(1, std::memory_order_relaxed);

  } catch (...) {
    active_threads_.fetch_sub(threads_needed, std::memory_order_relaxed);
    throw;
  }
}

}  // namespace gtopt
