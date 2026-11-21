#include "net/EventLoop.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/Log.hpp"
#include "net/ConnectionHandler.hpp"

namespace mc {
namespace {

std::string formatAddress(const sockaddr_storage& address, socklen_t length) {
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> service{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length,
                    host.data(), static_cast<socklen_t>(host.size()),
                    service.data(), static_cast<socklen_t>(service.size()),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown";
    }
    std::string result;
    if (address.ss_family == AF_INET6) {
        result = "[" + std::string(host.data()) + "]:" + service.data();
    } else {
        result = std::string(host.data()) + ":" + service.data();
    }
    return result;
}

int createListener(const std::string& host, std::uint16_t port, int index) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const std::string portText = std::to_string(port);
    const int status = getaddrinfo(host.empty() ? nullptr : host.c_str(), portText.c_str(), &hints, &results);
    if (status != 0) {
        Log::error("listener {}: getaddrinfo failed: {}", index, gai_strerror(status));
        return -1;
    }

    int listener = -1;
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, entry->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int enabled = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#ifdef SO_REUSEPORT
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
            Log::warn("listener {}: SO_REUSEPORT unavailable: {}", index, std::strerror(errno));
        }
#endif
        if (::bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            listener = fd;
            break;
        }
        ::close(fd);
    }
    freeaddrinfo(results);

    if (listener < 0) {
        Log::error("listener {}: failed to bind {}:{}", index, host, port);
    } else {
        Log::info("listener {} listening on {}:{}", index, host, port);
    }
    return listener;
}

}

EventLoop::EventLoop(ConnectionHandler& handler, std::string host, std::uint16_t port, std::size_t maxPacketSize, int index)
    : handler_(handler),
      host_(std::move(host)),
      port_(port),
      maxPacketSize_(maxPacketSize),
      index_(index) {}

EventLoop::~EventLoop() {
    stop();
}

bool EventLoop::init() {
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        Log::error("listener {}: epoll_create1 failed: {}", index_, std::strerror(errno));
        return false;
    }

    listenFd_ = createListener(host_, port_, index_);
    if (listenFd_ < 0) {
        return false;
    }

    wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        Log::error("listener {}: eventfd failed: {}", index_, std::strerror(errno));
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listenFd_;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &event) != 0) {
        Log::error("listener {}: epoll add listener failed: {}", index_, std::strerror(errno));
        return false;
    }

    event.events = EPOLLIN;
    event.data.fd = wakeFd_;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &event) != 0) {
        Log::error("listener {}: epoll add wake fd failed: {}", index_, std::strerror(errno));
        return false;
    }

    return true;
}

void EventLoop::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void EventLoop::stop() {
    const bool wasRunning = running_.exchange(false);
    if (wasRunning) {
        wake();
    }
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    closeAll();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (wakeFd_ >= 0) {
        ::close(wakeFd_);
        wakeFd_ = -1;
    }
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
}

void EventLoop::setWriteInterest(int fd, bool enabled) {
    if (epollFd_ < 0 || fd < 0) {
        return;
    }
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP)
        | (enabled ? static_cast<std::uint32_t>(EPOLLOUT) : 0U);
    event.data.fd = fd;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &event) != 0 && errno != ENOENT) {
        Log::trace("listener {}: epoll mod {} failed: {}", index_, fd, std::strerror(errno));
    }
}

void EventLoop::post(std::function<void()> task) {
    {
        std::lock_guard lock(tasksMutex_);
        tasks_.push_back(std::move(task));
    }
    wake();
}

void EventLoop::wake() {
    if (wakeFd_ < 0) {
        return;
    }
    const std::uint64_t value = 1;
    const ssize_t result = ::write(wakeFd_, &value, sizeof(value));
    (void)result;
}

bool EventLoop::isLoopThread() const {
    return std::this_thread::get_id() == threadId_;
}

void EventLoop::run() {
    threadId_ = std::this_thread::get_id();
    Log::debug("listener {} thread started", index_);

    std::array<epoll_event, 256> events{};
    while (running_.load(std::memory_order_acquire)) {
        const int count = ::epoll_wait(epollFd_, events.data(), static_cast<int>(events.size()), 50);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            Log::error("listener {}: epoll_wait failed: {}", index_, std::strerror(errno));
            break;
        }

        for (int i = 0; i < count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t mask = events[static_cast<std::size_t>(i)].events;
            if (fd == wakeFd_) {
                drainWake();
                continue;
            }
            if (fd == listenFd_) {
                acceptNew();
                continue;
            }
            const auto it = connections_.find(fd);
            if (it == connections_.end()) {
                continue;
            }
            const auto connection = it->second;
            if ((mask & (EPOLLERR | EPOLLHUP)) != 0) {
                connection->requestClose();
            }
            if (!connection->dead() && (mask & EPOLLIN) != 0) {
                connection->onReadable();
            }
            if (!connection->dead() && (mask & EPOLLOUT) != 0) {
                connection->onWritable();
            }
            if (!connection->dead() && (mask & EPOLLRDHUP) != 0) {
                connection->requestClose();
            }
        }

        runTasks();

        const auto now = std::chrono::steady_clock::now();
        for (auto& [fd, connection] : connections_) {
            if (!connection->dead()) {
                connection->tick(now);
            }
        }
        for (auto& [fd, connection] : connections_) {
            if (!connection->dead()) {
                handler_.onTick(*connection);
            }
        }
        reap();
    }
    Log::debug("listener {} thread stopped", index_);
}

void EventLoop::acceptNew() {
    while (true) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int fd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&address), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            Log::warn("listener {}: accept failed: {}", index_, std::strerror(errno));
            return;
        }

        int enabled = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));

        const std::string remote = formatAddress(address, length);
        auto connection = std::make_shared<Connection>(fd, *this, handler_, remote, maxPacketSize_);

        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = fd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            Log::warn("listener {}: epoll add connection failed: {}", index_, std::strerror(errno));
            connection->closeSocket();
            continue;
        }

        connections_[fd] = connection;
        handler_.onOpen(*connection);
    }
}

void EventLoop::drainWake() {
    std::uint64_t value = 0;
    while (::read(wakeFd_, &value, sizeof(value)) > 0) {
    }
}

void EventLoop::runTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(tasksMutex_);
        tasks.swap(tasks_);
    }
    for (auto& task : tasks) {
        try {
            task();
        } catch (const std::exception& error) {
            Log::error("loop task failed: {}", error.what());
        } catch (...) {
            Log::error("loop task failed with unknown error");
        }
    }
}

void EventLoop::reap() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        const auto& connection = it->second;
        if (connection->dead() || (connection->closeAfterFlush() && connection->outEmpty())) {
            const int fd = connection->fd();
            if (fd >= 0) {
                ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
            }
            connection->closeSocket();
            handler_.onClose(*connection);
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void EventLoop::closeAll() {
    for (auto& [fd, connection] : connections_) {
        if (fd >= 0 && epollFd_ >= 0) {
            ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        connection->closeSocket();
        handler_.onClose(*connection);
    }
    connections_.clear();
}

}
