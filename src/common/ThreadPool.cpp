#include "common/ThreadPool.hpp"

#include <exception>
#include <utility>

#include "common/Log.hpp"

namespace mc {

ThreadPool::ThreadPool(std::size_t threads) {
    const std::size_t count = threads == 0 ? 1 : threads;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        tasks_.push(std::move(task));
    }
    condition_.notify_one();
}

void ThreadPool::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ThreadPool::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& error) {
            Log::error("worker task failed: {}", error.what());
        } catch (...) {
            Log::error("worker task failed with unknown error");
        }
    }
}

}
