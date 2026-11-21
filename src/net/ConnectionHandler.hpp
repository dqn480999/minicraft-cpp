#pragma once

#include <cstdint>

namespace mc {

class Connection;
class ByteBuffer;

class ConnectionHandler {
public:
    virtual ~ConnectionHandler() = default;

    virtual void onOpen(Connection& connection) = 0;
    virtual void onPacket(Connection& connection, std::int32_t packetId, ByteBuffer& payload) = 0;
    virtual void onTick(Connection& connection) = 0;
    virtual void onClose(Connection& connection) = 0;
    virtual void onLegacyPing(Connection& connection) = 0;
};

}
