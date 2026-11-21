#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Config.hpp"

namespace mc {

class ConnectionHandler;
class EventLoop;

class TcpServer {
public:
    TcpServer(ConnectionHandler& handler, const ServerConfig& config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void stop();

private:
    ConnectionHandler& handler_;
    std::string host_;
    std::uint16_t port_;
    std::size_t maxPacketSize_;
    int threadCount_;
    bool pinThreads_;
    std::vector<std::unique_ptr<EventLoop>> loops_;
};

}
