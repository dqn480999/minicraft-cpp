#include "net/TcpServer.hpp"

#include <algorithm>
#include <thread>

#include "common/Log.hpp"
#include "net/EventLoop.hpp"

namespace mc {

TcpServer::TcpServer(ConnectionHandler& handler, const ServerConfig& config)
    : handler_(handler),
      host_(config.host),
      port_(config.port),
      maxPacketSize_(config.maxPacketSize),
      pinThreads_(config.pinIoThreads) {
    const auto hardware = std::thread::hardware_concurrency();
    const int automatic = static_cast<int>(std::clamp<unsigned int>(hardware == 0 ? 2U : hardware, 1U, 16U));
    threadCount_ = config.ioThreads > 0 ? std::clamp(config.ioThreads, 1, 64) : automatic;
}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    if (!loops_.empty()) {
        return true;
    }
    Log::info("starting {} io threads on {}:{}", threadCount_, host_, port_);
    loops_.reserve(static_cast<std::size_t>(threadCount_));
    for (int i = 0; i < threadCount_; ++i) {
        auto loop = std::make_unique<EventLoop>(handler_, host_, port_, maxPacketSize_, i, pinThreads_);
        if (!loop->init()) {
            Log::error("failed to initialize io thread {}", i);
            stop();
            return false;
        }
        loops_.push_back(std::move(loop));
    }
    for (auto& loop : loops_) {
        loop->start();
    }
    return true;
}

void TcpServer::stop() {
    for (auto& loop : loops_) {
        loop->stop();
    }
    loops_.clear();
}

}
