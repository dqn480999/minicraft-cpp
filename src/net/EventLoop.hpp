#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/Connection.hpp"

namespace mc {

class ConnectionHandler;

class EventLoop {
public:
    EventLoop(ConnectionHandler& handler, std::string host, std::uint16_t port, std::size_t maxPacketSize, int index);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool init();
    void start();
    void stop();

    void setWriteInterest(int fd, bool enabled);
    void post(std::function<void()> task);
    void wake();
    bool isLoopThread() const;

private:
    void run();
    void acceptNew();
    void drainWake();
    void runTasks();
    void reap();
    void closeAll();

    ConnectionHandler& handler_;
    std::string host_;
    std::uint16_t port_;
    std::size_t maxPacketSize_;
    int index_;

    int epollFd_{-1};
    int listenFd_{-1};
    int wakeFd_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::thread::id threadId_{};

    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::vector<std::function<void()>> tasks_;
    std::mutex tasksMutex_;
};

}
