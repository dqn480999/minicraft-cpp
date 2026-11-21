#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "common/Types.hpp"
#include "net/ByteBuffer.hpp"
#include "net/Zlib.hpp"
#include "protocol/ItemStack.hpp"
#include "world/Blocks.hpp"

namespace {

class ProbeClient {
public:
    ~ProbeClient() {
        closeSocket();
    }

    bool connect(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        const std::string portText = std::to_string(port);
        if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &results) != 0) {
            return false;
        }
        for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
            const int candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (candidate < 0) {
                continue;
            }
            if (::connect(candidate, entry->ai_addr, entry->ai_addrlen) == 0) {
                fd_ = candidate;
                break;
            }
            ::close(candidate);
        }
        freeaddrinfo(results);
        return fd_ >= 0;
    }

    void closeSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void sendPacket(mc::ByteBuffer body) {
        mc::ByteBuffer payload;
        if (compressionEnabled_) {
            if (static_cast<int>(body.size()) >= compressionThreshold_) {
                const mc::Bytes compressed = mc::zlibCompress(body.view());
                payload.writeVarInt(static_cast<std::int32_t>(body.size()));
                payload.writeBytes(compressed);
            } else {
                payload.writeVarInt(0);
                payload.writeBytes(body.view());
            }
        } else {
            payload.writeBytes(body.view());
        }

        mc::ByteBuffer frame;
        frame.writeVarInt(static_cast<std::int32_t>(payload.size()));
        frame.append(payload);
        const mc::Bytes bytes = frame.release();
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = ::send(fd_, bytes.data() + offset, bytes.size() - offset, 0);
            if (written <= 0) {
                throw std::runtime_error("send failed");
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    bool readPacket(mc::ByteBuffer& output) {
        std::int32_t length = 0;
        if (!readVarInt(length)) {
            return false;
        }
        if (length <= 0 || length > 4 * 1024 * 1024) {
            return false;
        }
        mc::Bytes frame(static_cast<std::size_t>(length));
        if (!readExact(frame.data(), frame.size())) {
            return false;
        }

        mc::ByteBuffer buffer(std::move(frame));
        if (compressionEnabled_) {
            const std::int32_t uncompressed = buffer.readVarInt();
            if (uncompressed != 0) {
                const mc::Bytes compressed = buffer.readRemaining();
                output = mc::ByteBuffer(mc::zlibDecompress(compressed, static_cast<std::size_t>(uncompressed), 4 * 1024 * 1024));
            } else {
                output = std::move(buffer);
            }
        } else {
            output = std::move(buffer);
        }
        return true;
    }

    void enableCompression(int threshold) {
        compressionEnabled_ = threshold >= 0;
        compressionThreshold_ = threshold;
    }

    int fd() const {
        return fd_;
    }

    bool writeRaw(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const mc::u8*>(data);
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t written = ::send(fd_, bytes + offset, size - offset, 0);
            if (written <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool readRaw(void* destination, std::size_t size) {
        return readExact(destination, size);
    }

private:
    bool readExact(void* destination, std::size_t size) {
        auto* bytes = static_cast<mc::u8*>(destination);
        std::size_t offset = 0;
        while (offset < size) {
            pollfd descriptor{fd_, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 5000);
            if (ready <= 0) {
                return false;
            }
            const ssize_t received = ::recv(fd_, bytes + offset, size - offset, 0);
            if (received <= 0) {
                if (received < 0 && errno == EINTR) {
                    continue;
                }
                return false;
            }
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    bool readByte(mc::u8& value) {
        return readExact(&value, 1);
    }

    bool readVarInt(std::int32_t& value) {
        std::uint32_t result = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            mc::u8 byte = 0;
            if (!readByte(byte)) {
                return false;
            }
            result |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                value = static_cast<std::int32_t>(result);
                return true;
            }
        }
        return false;
    }

    int fd_{-1};
    bool compressionEnabled_{false};
    int compressionThreshold_{0};
};

bool doLegacyPing(const std::string& host, std::uint16_t port) {
    ProbeClient client;
    if (!client.connect(host, port)) {
        std::cerr << "legacy: connect failed\n";
        return false;
    }
    const mc::u8 request[2] = {0xFE, 0x01};
    if (!client.writeRaw(request, sizeof(request))) {
        return false;
    }
    mc::u8 header = 0;
    if (!client.readRaw(&header, 1) || header != 0xFF) {
        std::cerr << "legacy: invalid response\n";
        return false;
    }
    mc::u8 lengthBytes[2] = {0, 0};
    if (!client.readRaw(lengthBytes, sizeof(lengthBytes))) {
        return false;
    }
    const std::uint16_t length = static_cast<std::uint16_t>((lengthBytes[0] << 8) | lengthBytes[1]);
    std::vector<mc::u8> payload(length);
    if (length > 0 && !client.readRaw(payload.data(), payload.size())) {
        return false;
    }
    std::string text;
    for (std::size_t i = 0; i + 1 < payload.size(); i += 2) {
        const unsigned code = (payload[i] << 8) | payload[i + 1];
        text.push_back(code < 128 ? static_cast<char>(code) : '?');
    }
    if (text.find("1.8.9") == std::string::npos) {
        std::cerr << "legacy: unexpected payload " << text << '\n';
        return false;
    }
    std::cout << "legacy ping PASS " << text << '\n';
    return true;
}

bool doStatus(const std::string& host, std::uint16_t port, std::int32_t protocol) {
    ProbeClient client;
    if (!client.connect(host, port)) {
        std::cerr << "status: connect failed\n";
        return false;
    }

    mc::ByteBuffer handshake;
    handshake.writeVarInt(0x00);
    handshake.writeVarInt(protocol);
    handshake.writeString(host);
    handshake.writeU16(port);
    handshake.writeVarInt(1);
    client.sendPacket(std::move(handshake));

    mc::ByteBuffer request;
    request.writeVarInt(0x00);
    client.sendPacket(std::move(request));

    mc::ByteBuffer response;
    if (!client.readPacket(response)) {
        std::cerr << "status: read failed\n";
        return false;
    }
    const std::int32_t packetId = response.readVarInt();
    if (packetId != 0x00) {
        std::cerr << "status: unexpected packet " << packetId << '\n';
        return false;
    }
    const std::string json = response.readString(32767);
    std::cout << json << '\n';

    mc::ByteBuffer ping;
    ping.writeVarInt(0x01);
    ping.writeI64(123456789);
    client.sendPacket(std::move(ping));

    mc::ByteBuffer pong;
    if (client.readPacket(pong)) {
        const std::int32_t pongId = pong.readVarInt();
        if (pongId == 0x01) {
            const std::int64_t payload = pong.readI64();
            std::cout << "pong " << payload << '\n';
        }
    }
    return true;
}

bool expectBlockChange(ProbeClient& client, std::int32_t x, std::int32_t y, std::int32_t z, mc::u16 expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x23) {
            std::int32_t blockX = 0;
            std::int32_t blockY = 0;
            std::int32_t blockZ = 0;
            packet.readPosition(blockX, blockY, blockZ);
            const mc::u16 block = static_cast<mc::u16>(packet.readVarInt());
            if (blockX == x && blockY == y && blockZ == z) {
                if (block != expected) {
                    std::cerr << "block test: unexpected state " << block << " expected " << expected << '\n';
                    return false;
                }
                return true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            std::cerr << "block test: disconnected " << packet.readString(32767) << '\n';
            return false;
        }
    }
    return false;
}

bool doDropTest(ProbeClient& client) {
    const auto click = [&](std::int16_t slot, std::int16_t action) {
        mc::ByteBuffer packet;
        packet.writeVarInt(0x0E);
        packet.writeU8(0);
        packet.writeI16(slot);
        packet.writeI8(0);
        packet.writeI16(action);
        packet.writeI8(0);
        packet.writeI16(-1);
        client.sendPacket(std::move(packet));
    };
    click(37, 1);
    click(36, 2);
    click(37, 3);

    bool clicksOk = false;
    bool dropOk = false;
    bool cursorPacket = false;
    int confirmations = 0;
    bool dropSent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x32) {
            ++confirmations;
        } else if (packetId == 0x2F) {
            const mc::i8 windowId = packet.readI8();
            const std::int16_t slot = packet.readI16();
            mc::readItemStack(packet);
            if (windowId == -1 && slot == -1) {
                cursorPacket = true;
            }
        } else if (packetId == 0x0E) {
            packet.readVarInt();
            const mc::i8 type = packet.readI8();
            if (type == 2 && dropSent) {
                dropOk = true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }

        if (!clicksOk && confirmations >= 3) {
            if (!cursorPacket) {
                std::cerr << "cursor packet FAIL\n";
                return false;
            }
            clicksOk = true;
            std::cout << "cursor packet PASS\n";
            std::cout << "inventory click PASS\n";
            mc::ByteBuffer drop;
            drop.writeVarInt(0x07);
            drop.writeVarInt(4);
            drop.writePosition(0, 0, 0);
            drop.writeI8(0);
            client.sendPacket(std::move(drop));
            dropSent = true;
        }
        if (clicksOk && dropOk) {
            std::cout << "drop PASS\n";
            return true;
        }
    }
    std::cerr << "inventory click/drop FAIL confirmations=" << confirmations
              << " cursor=" << cursorPacket << " drop=" << dropOk << '\n';
    return false;
}

bool doStackDropTest(ProbeClient& client) {
    mc::ByteBuffer held;
    held.writeVarInt(0x09);
    held.writeI16(7);
    client.sendPacket(std::move(held));

    const auto readDrop = [&](std::int32_t expectedCount) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            mc::ByteBuffer packet;
            if (!client.readPacket(packet)) {
                return false;
            }
            const std::int32_t packetId = packet.readVarInt();
            if (packetId == 0x0E) {
                const std::int32_t entityId = packet.readVarInt();
                const mc::i8 type = packet.readI8();
                packet.readI32();
                packet.readI32();
                packet.readI32();
                packet.readI8();
                packet.readI8();
                const std::int32_t objectData = packet.readI32();
                if (objectData != 0) {
                    packet.readI16();
                    packet.readI16();
                    packet.readI16();
                }
                (void)entityId;
                (void)type;
            } else if (packetId == 0x1C) {
                packet.readVarInt();
                const mc::u8 key = packet.readU8();
                const mc::ItemStack stack = mc::readItemStack(packet);
                if (key == 0xAA && stack.id == 5 && stack.count == expectedCount) {
                    return true;
                }
            } else if (packetId == 0x00) {
                const std::int32_t keepAlive = packet.readVarInt();
                mc::ByteBuffer reply;
                reply.writeVarInt(0x00);
                reply.writeVarInt(keepAlive);
                client.sendPacket(std::move(reply));
            } else if (packetId == 0x40) {
                return false;
            }
        }
        return false;
    };

    mc::ByteBuffer one;
    one.writeVarInt(0x07);
    one.writeVarInt(4);
    one.writePosition(0, 0, 0);
    one.writeI8(0);
    client.sendPacket(std::move(one));
    if (!readDrop(1)) {
        std::cerr << "drop one FAIL\n";
        return false;
    }
    std::cout << "drop one PASS\n";

    mc::ByteBuffer all;
    all.writeVarInt(0x07);
    all.writeVarInt(3);
    all.writePosition(0, 0, 0);
    all.writeI8(0);
    client.sendPacket(std::move(all));
    if (!readDrop(63)) {
        std::cerr << "drop stack FAIL\n";
        return false;
    }
    std::cout << "drop stack PASS\n";
    return true;
}

bool doTorchTest(ProbeClient& client, std::int32_t baseX, std::int32_t baseZ) {
    mc::ByteBuffer held;
    held.writeVarInt(0x09);
    held.writeI16(8);
    client.sendPacket(std::move(held));

    mc::ByteBuffer place;
    place.writeVarInt(0x08);
    place.writePosition(baseX, 3, baseZ);
    place.writeI8(1);
    place.writeI16(50);
    place.writeI8(1);
    place.writeI16(0);
    client.sendPacket(std::move(place));

    mc::ByteBuffer dig;
    dig.writeVarInt(0x07);
    dig.writeVarInt(2);
    dig.writePosition(baseX, 4, baseZ);
    dig.writeI8(0);
    client.sendPacket(std::move(dig));

    bool broken = false;
    bool torchDrop = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x23) {
            std::int32_t x = 0;
            std::int32_t y = 0;
            std::int32_t z = 0;
            packet.readPosition(x, y, z);
            const mc::u16 block = static_cast<mc::u16>(packet.readVarInt());
            if (x == baseX && y == 4 && z == baseZ && block == mc::kAir) {
                broken = true;
            }
        } else if (packetId == 0x0E) {
            const std::int32_t entityId = packet.readVarInt();
            const mc::i8 type = packet.readI8();
            packet.readI32();
            packet.readI32();
            packet.readI32();
            packet.readI8();
            packet.readI8();
            const std::int32_t objectData = packet.readI32();
            if (objectData != 0) {
                packet.readI16();
                packet.readI16();
                packet.readI16();
            }
            (void)entityId;
            (void)type;
        } else if (packetId == 0x1C) {
            packet.readVarInt();
            const mc::u8 key = packet.readU8();
            const mc::ItemStack stack = mc::readItemStack(packet);
            if (key == 0xAA && stack.id == 50) {
                torchDrop = true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
        if (broken && torchDrop) {
            std::cout << "torch place/drop PASS\n";
            return true;
        }
    }
    std::cerr << "torch FAIL broken=" << broken << " drop=" << torchDrop << '\n';
    return false;
}

bool doSurvivalBreakTest(ProbeClient& client, std::int32_t offsetX, std::int32_t offsetZ) {
    mc::ByteBuffer move;
    move.writeVarInt(0x04);
    move.writeF64(8.5 + static_cast<double>(offsetX));
    move.writeF64(4.0);
    move.writeF64(8.5 + static_cast<double>(offsetZ));
    move.writeBool(true);
    client.sendPacket(std::move(move));

    mc::ByteBuffer dig;
    dig.writeVarInt(0x07);
    dig.writeVarInt(2);
    dig.writePosition(8 + offsetX, 3, 8 + offsetZ);
    dig.writeI8(1);
    client.sendPacket(std::move(dig));

    bool broken = false;
    bool dropped = false;
    bool collected = false;
    std::int32_t droppedEntityId = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x23) {
            std::int32_t blockX = 0;
            std::int32_t blockY = 0;
            std::int32_t blockZ = 0;
            packet.readPosition(blockX, blockY, blockZ);
            const mc::u16 block = static_cast<mc::u16>(packet.readVarInt());
            if (blockX == 8 + offsetX && blockY == 3 && blockZ == 8 + offsetZ && block == mc::kAir) {
                broken = true;
            }
        } else if (packetId == 0x0E) {
            const std::int32_t entityId = packet.readVarInt();
            const mc::i8 type = packet.readI8();
            packet.readI32();
            packet.readI32();
            packet.readI32();
            packet.readI8();
            packet.readI8();
            const std::int32_t objectData = packet.readI32();
            if (objectData != 0) {
                packet.readI16();
                packet.readI16();
                packet.readI16();
            }
            if (type == 2) {
                dropped = true;
                droppedEntityId = entityId;
            }
        } else if (packetId == 0x0D) {
            packet.readVarInt();
            packet.readVarInt();
            collected = true;
        } else if (packetId == 0x13) {
            const std::int32_t count = packet.readVarInt();
            for (std::int32_t i = 0; i < count; ++i) {
                if (packet.readVarInt() == droppedEntityId) {
                    collected = true;
                }
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
        if (broken && dropped && collected) {
            std::cout << "survival break PASS\n";
            std::cout << "item drop PASS\n";
            std::cout << "item pickup PASS\n";
            return doDropTest(client)
                && doStackDropTest(client)
                && doTorchTest(client, 8 + offsetX + 2, 8 + offsetZ);
        }
    }
    std::cerr << "survival break FAIL broken=" << broken << " drop=" << dropped << " pickup=" << collected << '\n';
    return false;
}

bool doBlockTest(ProbeClient& client, std::int32_t offsetX, std::int32_t offsetZ) {
    mc::ByteBuffer place;
    place.writeVarInt(0x08);
    place.writePosition(8 + offsetX, 3, 8 + offsetZ);
    place.writeI8(1);
    place.writeI16(1);
    place.writeI8(1);
    place.writeI16(0);
    client.sendPacket(std::move(place));
    if (!expectBlockChange(client, 8 + offsetX, 4, 8 + offsetZ, static_cast<mc::u16>(mc::kStone))) {
        std::cerr << "block place FAIL\n";
        return false;
    }
    std::cout << "block place PASS\n";

    mc::ByteBuffer dig;
    dig.writeVarInt(0x07);
    dig.writeVarInt(2);
    dig.writePosition(8 + offsetX, 4, 8 + offsetZ);
    dig.writeI8(0);
    client.sendPacket(std::move(dig));
    if (!expectBlockChange(client, 8 + offsetX, 4, 8 + offsetZ, mc::kAir)) {
        std::cerr << "block break FAIL\n";
        return false;
    }
    std::cout << "block break PASS\n";
    return true;
}

bool doSandTest(ProbeClient& client, std::int32_t x, std::int32_t z) {
    mc::ByteBuffer command;
    command.writeVarInt(0x01);
    command.writeString("/setblock " + std::to_string(x) + " 10 " + std::to_string(z) + " 12");
    client.sendPacket(std::move(command));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x23) {
            std::int32_t blockX = 0;
            std::int32_t blockY = 0;
            std::int32_t blockZ = 0;
            packet.readPosition(blockX, blockY, blockZ);
            const mc::u16 block = static_cast<mc::u16>(packet.readVarInt());
            if (blockX == x && blockY == 4 && blockZ == z && block == mc::kSand) {
                std::cout << "sand physics PASS\n";
                return true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
    }
    std::cerr << "sand physics FAIL\n";
    return false;
}

bool parseEntityMetadata(mc::ByteBuffer& packet) {
    while (packet.remaining() > 0) {
        const mc::u8 key = packet.readU8();
        if (key == 0x7F) {
            return true;
        }
        const int type = key >> 5;
        switch (type) {
        case 0:
            packet.readU8();
            break;
        case 1:
            packet.readI16();
            break;
        case 2:
            packet.readI32();
            break;
        case 3:
            packet.readF32();
            break;
        case 4:
            packet.readString(32767);
            break;
        case 5:
            mc::readItemStack(packet);
            break;
        case 6:
            packet.readI32();
            packet.readI32();
            packet.readI32();
            break;
        case 7:
            packet.readF32();
            packet.readF32();
            packet.readF32();
            break;
        default:
            return false;
        }
    }
    return false;
}

bool doTimeTest(ProbeClient& client) {
    std::int64_t firstTime = -1;
    bool lowFood = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x03) {
            packet.readI64();
            const std::int64_t time = packet.readI64();
            if (firstTime < 0) {
                firstTime = time;
            } else if (time != firstTime) {
                std::cout << "time PASS\n";
                if (lowFood) {
                    std::cout << "hunger PASS\n";
                }
                return true;
            }
        } else if (packetId == 0x06) {
            packet.readF32();
            const std::int32_t food = packet.readVarInt();
            packet.readF32();
            if (food < 20) {
                lowFood = true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
    }
    std::cerr << "time FAIL\n";
    return false;
}

bool doEatTest(ProbeClient& client) {
    mc::ByteBuffer itemPacket;
    itemPacket.writeVarInt(0x10);
    itemPacket.writeI16(36);
    mc::writeItemStack(itemPacket, mc::ItemStack{260, 1, 0});
    client.sendPacket(std::move(itemPacket));

    mc::ByteBuffer foodCommand;
    foodCommand.writeVarInt(0x01);
    foodCommand.writeString("/setfood TestBot 5");
    client.sendPacket(std::move(foodCommand));

    mc::ByteBuffer use;
    use.writeVarInt(0x08);
    use.writePosition(0, 0, 0);
    use.writeI8(-1);
    use.writeI16(260);
    use.writeI8(1);
    use.writeI16(0);
    client.sendPacket(std::move(use));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x06) {
            packet.readF32();
            const std::int32_t food = packet.readVarInt();
            packet.readF32();
            if (food >= 9) {
                std::cout << "eating PASS\n";
                return true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
    }
    std::cerr << "eating FAIL\n";
    return false;
}

bool doMobTest(ProbeClient& client) {
    mc::ByteBuffer command;
    command.writeVarInt(0x01);
    command.writeString("/summon 54");
    client.sendPacket(std::move(command));

    std::int32_t mobId = 0;
    bool attacked = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x0F && mobId == 0) {
            const std::int32_t entityId = packet.readVarInt();
            const mc::u8 type = packet.readU8();
            packet.readI32();
            packet.readI32();
            packet.readI32();
            packet.readI8();
            packet.readI8();
            packet.readI8();
            packet.readI16();
            packet.readI16();
            packet.readI16();
            if (!parseEntityMetadata(packet) || packet.remaining() != 0) {
                return false;
            }
            if (type == 54) {
                mobId = entityId;
                mc::ByteBuffer attack;
                attack.writeVarInt(0x02);
                attack.writeVarInt(mobId);
                attack.writeVarInt(1);
                client.sendPacket(std::move(attack));
            }
        } else if (packetId == 0x1A) {
            const std::int32_t entityId = packet.readI32();
            packet.readI8();
            if (entityId == mobId) {
                attacked = true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
        if (mobId != 0 && attacked) {
            std::cout << "mob attack PASS\n";
            return true;
        }
    }
    std::cerr << "mob attack FAIL\n";
    return false;
}

bool doFluidTest(ProbeClient& client, std::int32_t x, std::int32_t z) {
    mc::ByteBuffer command;
    command.writeVarInt(0x01);
    command.writeString("/setblock " + std::to_string(x) + " 6 " + std::to_string(z) + " 9");
    client.sendPacket(std::move(command));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x23) {
            std::int32_t blockX = 0;
            std::int32_t blockY = 0;
            std::int32_t blockZ = 0;
            packet.readPosition(blockX, blockY, blockZ);
            const mc::u16 block = static_cast<mc::u16>(packet.readVarInt());
            const mc::u16 id = static_cast<mc::u16>(block >> 4);
            if (blockX == x + 3 && blockY == 6 && blockZ == z && (id == 8 || id == 9)) {
                std::cout << "fluid spread PASS\n";
                return true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
    }
    std::cerr << "fluid spread FAIL\n";
    return false;
}

bool doDamageTest(ProbeClient& client) {
    mc::ByteBuffer kill;
    kill.writeVarInt(0x01);
    kill.writeString("/kill TestBot");
    client.sendPacket(std::move(kill));

    bool dead = false;
    bool respawned = false;
    bool respawnRequested = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();
        if (packetId == 0x06) {
            const float health = packet.readF32();
            packet.readVarInt();
            packet.readF32();
            if (health <= 0.0F) {
                dead = true;
                if (!respawnRequested) {
                    respawnRequested = true;
                    mc::ByteBuffer respawn;
                    respawn.writeVarInt(0x16);
                    respawn.writeVarInt(0);
                    client.sendPacket(std::move(respawn));
                }
            }
        } else if (packetId == 0x07) {
            respawned = true;
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            return false;
        }
        if (dead && respawned) {
            std::cout << "damage/respawn PASS\n";
            return true;
        }
    }
    std::cerr << "damage/respawn FAIL dead=" << dead << " respawn=" << respawned << '\n';
    return false;
}

bool doLogin(const std::string& host, std::uint16_t port, std::int32_t protocol, const std::string& username) {
    ProbeClient client;
    if (!client.connect(host, port)) {
        std::cerr << "login: connect failed\n";
        return false;
    }

    mc::ByteBuffer handshake;
    handshake.writeVarInt(0x00);
    handshake.writeVarInt(protocol);
    handshake.writeString(host);
    handshake.writeU16(port);
    handshake.writeVarInt(2);
    client.sendPacket(std::move(handshake));

    mc::ByteBuffer loginStart;
    loginStart.writeVarInt(0x00);
    loginStart.writeString(username);
    client.sendPacket(std::move(loginStart));

    bool success = false;
    bool joinGame = false;
    bool helpSent = false;
    bool brandSent = false;
    bool brandOk = false;
    int chunks = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (std::chrono::steady_clock::now() < deadline) {
        mc::ByteBuffer packet;
        if (!client.readPacket(packet)) {
            std::cerr << "login: read failed\n";
            return false;
        }
        const std::int32_t packetId = packet.readVarInt();

        if (packetId == 0x03 && !success) {
            const std::int32_t threshold = packet.readVarInt();
            client.enableCompression(threshold);
            std::cout << "compression " << threshold << '\n';
        } else if (packetId == 0x00 && !success && !joinGame) {
            std::cout << "disconnect " << packet.readString(32767) << '\n';
            return false;
        } else if (packetId == 0x02 && !success) {
            const std::string uuid = packet.readString(64);
            const std::string name = packet.readString(32);
            std::cout << "login success " << name << ' ' << uuid << '\n';
            success = true;
        } else if (packetId == 0x01) {
            const std::int32_t entityId = packet.readI32();
            const mc::u8 gameMode = packet.readU8();
            const mc::i8 dimension = packet.readI8();
            const mc::u8 difficulty = packet.readU8();
            const mc::u8 maxPlayers = packet.readU8();
            const std::string levelType = packet.readString(32);
            packet.readBool();
            std::cout << "join game entity=" << entityId
                      << " mode=" << static_cast<int>(gameMode)
                      << " dimension=" << static_cast<int>(dimension)
                      << " difficulty=" << static_cast<int>(difficulty)
                      << " max=" << static_cast<int>(maxPlayers)
                      << " type=" << levelType << '\n';
            joinGame = true;
        } else if (packetId == 0x30) {
            packet.readU8();
            const std::int16_t count = packet.readI16();
            int items = 0;
            for (std::int16_t i = 0; i < count; ++i) {
                const mc::ItemStack stack = mc::readItemStack(packet);
                if (!stack.empty()) {
                    ++items;
                }
            }
            std::cout << "inventory slots=" << count << " items=" << items << '\n';
        } else if (packetId == 0x2F) {
            packet.readI8();
            packet.readI16();
            mc::readItemStack(packet);
        } else if (packetId == 0x09) {
            packet.readI8();
        } else if (packetId == 0x0C) {
            packet.readVarInt();
            packet.readBytes(16);
            packet.readI32();
            packet.readI32();
            packet.readI32();
            packet.readI8();
            packet.readI8();
            packet.readI16();
            if (!parseEntityMetadata(packet) || packet.remaining() != 0) {
                std::cerr << "spawn player packet malformed\n";
                return false;
            }
        } else if (packetId == 0x0F) {
            packet.readVarInt();
            packet.readU8();
            packet.readI32();
            packet.readI32();
            packet.readI32();
            packet.readI8();
            packet.readI8();
            packet.readI8();
            packet.readI16();
            packet.readI16();
            packet.readI16();
            if (!parseEntityMetadata(packet) || packet.remaining() != 0) {
                std::cerr << "mob spawn packet malformed\n";
                return false;
            }
        } else if (packetId == 0x3F) {
            const std::string channel = packet.readString(64);
            if (channel == "MC|Brand") {
                const std::string brand = packet.readString(64);
                if (brand == "minicraft-cpp") {
                    brandOk = true;
                    std::cout << "brand PASS\n";
                }
            }
        } else if (packetId == 0x21) {
            const std::int32_t chunkX = packet.readI32();
            const std::int32_t chunkZ = packet.readI32();
            const bool groundUp = packet.readBool();
            const mc::u16 mask = packet.readU16();
            const std::int32_t size = packet.readVarInt();
            const mc::Bytes data = packet.readBytes(static_cast<std::size_t>(size));
            ++chunks;
            if (chunks <= 3) {
                std::cout << "chunk " << chunkX << ',' << chunkZ
                          << " groundUp=" << groundUp
                          << " mask=0x" << std::hex << mask << std::dec
                          << " bytes=" << data.size() << '\n';
            }
        } else if (packetId == 0x02) {
            const std::string json = packet.readString(32767);
            const mc::i8 position = packet.readI8();
            if (json.find("Commands:") != std::string::npos) {
                std::cout << "command PASS position=" << static_cast<int>(position) << '\n';
                const std::size_t hash = std::hash<std::string>{}(username);
                const auto offsetX = static_cast<std::int32_t>(hash % 11) - 5;
                const auto offsetZ = static_cast<std::int32_t>((hash / 11) % 11) - 5;
                if (!doTimeTest(client)) {
                    return false;
                }
                if (chunks > 200) {
                    if (!doBlockTest(client, offsetX, offsetZ)
                        || !doSandTest(client, 8 + offsetX, 8 + offsetZ)
                        || !doFluidTest(client, 8 + offsetX + 3, 8 + offsetZ)
                        || !doDamageTest(client)
                        || !doMobTest(client)
                        || !doEatTest(client)) {
                        return false;
                    }
                } else if (!doSurvivalBreakTest(client, offsetX, offsetZ)) {
                    return false;
                }
                std::cout << "probe PASS chunks=" << chunks << '\n';
                return true;
            }
        } else if (packetId == 0x00) {
            const std::int32_t keepAlive = packet.readVarInt();
            mc::ByteBuffer reply;
            reply.writeVarInt(0x00);
            reply.writeVarInt(keepAlive);
            client.sendPacket(std::move(reply));
        } else if (packetId == 0x40) {
            std::cout << "play disconnect " << packet.readString(32767) << '\n';
            return false;
        }

        if (joinGame && !brandSent) {
            mc::ByteBuffer brand;
            brand.writeVarInt(0x17);
            brand.writeString("MC|Brand");
            brand.writeString("probe");
            client.sendPacket(std::move(brand));
            brandSent = true;
        }

        if (success && joinGame && chunks > 0 && !helpSent) {
            mc::ByteBuffer gameModeCommand;
            gameModeCommand.writeVarInt(0x01);
            gameModeCommand.writeString("/gamemode 1");
            client.sendPacket(std::move(gameModeCommand));

            mc::ByteBuffer timeCommand;
            timeCommand.writeVarInt(0x01);
            timeCommand.writeString("/time set 1000");
            client.sendPacket(std::move(timeCommand));

            mc::ByteBuffer helpCommand;
            helpCommand.writeVarInt(0x01);
            helpCommand.writeString("/help");
            client.sendPacket(std::move(helpCommand));
            helpSent = true;
        }
    }

    std::cerr << "probe FAIL success=" << success << " join=" << joinGame << " chunks=" << chunks << '\n';
    return false;
}

}

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const auto port = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 25565);
    const std::string username = argc > 3 ? argv[3] : "TestBot";

    try {
        if (!doLegacyPing(host, port)) {
            return 1;
        }
        if (!doStatus(host, port, 47)) {
            return 1;
        }
        if (!doLogin(host, port, 47, username)) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "probe error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
