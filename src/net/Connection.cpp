#include "net/Connection.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

#include "common/Log.hpp"
#include "net/ConnectionHandler.hpp"
#include "net/Encryption.hpp"
#include "net/EventLoop.hpp"
#include "net/Zlib.hpp"
#include "protocol/Chat.hpp"

namespace mc {
namespace {

std::atomic<std::int32_t> g_keepAliveCounter{1};

std::int32_t nextKeepAliveId() {
    return g_keepAliveCounter.fetch_add(1, std::memory_order_relaxed);
}

unsigned decodeUtf8(std::string_view text, std::size_t& index) {
    const auto first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) {
        return first;
    }
    int remaining = 0;
    unsigned code = 0;
    if ((first & 0xE0) == 0xC0) {
        remaining = 1;
        code = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        remaining = 2;
        code = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        remaining = 3;
        code = first & 0x07;
    } else {
        return '?';
    }
    for (int i = 0; i < remaining; ++i) {
        if (index >= text.size()) {
            return '?';
        }
        const auto next = static_cast<unsigned char>(text[index++]);
        if ((next & 0xC0) != 0x80) {
            return '?';
        }
        code = (code << 6) | (next & 0x3F);
    }
    return code;
}

void appendUtf16(std::vector<u16>& output, std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned code = decodeUtf8(text, index);
        if (code <= 0xFFFF) {
            output.push_back(static_cast<u16>(code));
        } else if (code <= 0x10FFFF) {
            const unsigned adjusted = code - 0x10000;
            output.push_back(static_cast<u16>(0xD800 | (adjusted >> 10)));
            output.push_back(static_cast<u16>(0xDC00 | (adjusted & 0x3FF)));
        } else {
            output.push_back('?');
        }
    }
}

}

Connection::Connection(int fd, EventLoop& loop, ConnectionHandler& handler, std::string remoteAddress, std::size_t maxPacketSize)
    : fd_(fd),
      loop_(loop),
      handler_(handler),
      remoteAddress_(std::move(remoteAddress)),
      maxPacketSize_(maxPacketSize),
      connectedAt_(std::chrono::steady_clock::now()),
      lastRead_(connectedAt_),
      lastKeepAliveSent_(connectedAt_) {}

Connection::~Connection() {
    closeSocket();
}

int Connection::fd() const {
    return fd_;
}

EventLoop& Connection::loop() {
    return loop_;
}

const std::string& Connection::remoteAddress() const {
    return remoteAddress_;
}

ConnectionState Connection::state() const {
    return state_.load(std::memory_order_acquire);
}

void Connection::setState(ConnectionState state) {
    state_.store(state, std::memory_order_release);
}

bool Connection::dead() const {
    return dead_.load(std::memory_order_acquire);
}

bool Connection::closeAfterFlush() const {
    return closeAfterFlush_.load(std::memory_order_acquire);
}

bool Connection::outEmpty() const {
    std::lock_guard lock(writeMutex_);
    return outputOffset_ >= output_.size();
}

void Connection::requestClose() {
    dead_.store(true, std::memory_order_release);
    loop_.wake();
}

void Connection::closeGracefully() {
    if (dead()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    closeRequestedNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), std::memory_order_relaxed);
    closeAfterFlush_.store(true, std::memory_order_release);
    loop_.wake();
}

void Connection::disconnect(const std::string& jsonReason) {
    if (dead()) {
        return;
    }
    const auto current = state();
    if (current == ConnectionState::Play) {
        send(0x40, [&](ByteBuffer& buffer) { buffer.writeString(jsonReason); });
    } else if (current == ConnectionState::Login) {
        send(0x00, [&](ByteBuffer& buffer) { buffer.writeString(jsonReason); });
    } else {
        requestClose();
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    closeRequestedNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), std::memory_order_relaxed);
    closeAfterFlush_.store(true, std::memory_order_release);
    loop_.wake();
}

void Connection::onReadable() {
    if (dead()) {
        return;
    }
    std::array<u8, 65536> buffer{};
    const std::size_t maxBuffered = maxPacketSize_ * 2 + buffer.size();
    while (!dead()) {
        const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            const auto count = static_cast<std::size_t>(received);
            lastRead_ = std::chrono::steady_clock::now();
            if (encryption_) {
                encryption_->decrypt(buffer.data(), count);
            }
            input_.append(buffer.data(), count);
            if (input_.remaining() > maxBuffered) {
                fail("Packet buffer overflow");
                return;
            }
            continue;
        }
        if (received == 0) {
            dead_.store(true, std::memory_order_release);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        dead_.store(true, std::memory_order_release);
        return;
    }
    processInput();
}

void Connection::onWritable() {
    if (dead()) {
        return;
    }
    std::lock_guard lock(writeMutex_);
    flushLocked();
}

void Connection::tick(std::chrono::steady_clock::time_point now) {
    if (dead()) {
        return;
    }

    if (closeAfterFlush()) {
        {
            std::lock_guard lock(writeMutex_);
            flushLocked();
        }
        if (outEmpty()) {
            dead_.store(true, std::memory_order_release);
        } else {
            const auto requestedNs = closeRequestedNs_.load(std::memory_order_relaxed);
            const auto requested = std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(requestedNs)));
            if (now - requested > std::chrono::seconds(5)) {
                dead_.store(true, std::memory_order_release);
            }
        }
        return;
    }

    const auto current = state();
    if (current == ConnectionState::Handshake || current == ConnectionState::Status || current == ConnectionState::Login) {
        if (now - connectedAt_ > std::chrono::seconds(30)) {
            dead_.store(true, std::memory_order_release);
        }
        return;
    }

    if (current == ConnectionState::Play) {
        if (awaitingKeepAlive_ && now - lastKeepAliveSent_ > std::chrono::milliseconds(timeoutMs_)) {
            fail("Timed out");
            return;
        }
        if (now - lastKeepAliveSent_ > std::chrono::seconds(15)) {
            keepAliveId_ = nextKeepAliveId();
            send(0x00, [&](ByteBuffer& buffer) { buffer.writeVarInt(keepAliveId_); });
            lastKeepAliveSent_ = now;
            awaitingKeepAlive_ = true;
        }
        std::lock_guard lock(writeMutex_);
        if (!output_.empty()) {
            flushLocked();
        }
    }
}

void Connection::sendPacket(ByteBuffer body) {
    if (dead() || closeAfterFlush()) {
        return;
    }

    const bool compression = compressionEnabled_.load(std::memory_order_acquire);
    const int threshold = compressionThreshold_.load(std::memory_order_acquire);

    ByteBuffer payload;
    if (compression) {
        if (static_cast<int>(body.size()) >= threshold) {
            const Bytes compressed = zlibCompress(body.view());
            payload.writeVarInt(static_cast<i32>(body.size()));
            payload.writeBytes(compressed);
        } else {
            payload.writeVarInt(0);
            payload.writeBytes(body.view());
        }
    } else {
        payload.writeBytes(body.view());
    }

    ByteBuffer frame;
    frame.writeVarInt(static_cast<i32>(payload.size()));
    frame.append(payload);
    Bytes bytes = frame.release();

    const bool external = !onLoopThread();
    {
        std::lock_guard lock(writeMutex_);
        if (dead() || closeAfterFlush()) {
            return;
        }
        if (encryption_) {
            encryption_->encrypt(bytes.data(), bytes.size());
        }
        output_.insert(output_.end(), bytes.begin(), bytes.end());
        if (output_.size() - outputOffset_ > maxOutputBuffer_) {
            dead_.store(true, std::memory_order_release);
            output_.clear();
            outputOffset_ = 0;
        } else {
            setWriteInterestLocked(true);
        }
    }
    if (external) {
        loop_.wake();
    }
}

void Connection::enableCompression(int threshold) {
    compressionThreshold_.store(threshold, std::memory_order_release);
    compressionEnabled_.store(threshold >= 0, std::memory_order_release);
}

void Connection::enableEncryption(std::span<const u8> sharedSecret) {
    auto encryption = std::make_unique<Encryption>(sharedSecret);
    std::lock_guard lock(writeMutex_);
    encryption_ = std::move(encryption);
}

void Connection::setProtocolVersion(std::int32_t version) {
    protocolVersion_ = version;
}

std::int32_t Connection::protocolVersion() const {
    return protocolVersion_;
}

void Connection::setVirtualHost(std::string host) {
    virtualHost_ = std::move(host);
}

const std::string& Connection::virtualHost() const {
    return virtualHost_;
}

void Connection::setIdentity(const Uuid& uuid, std::string username, std::vector<ProfileProperty> properties) {
    uuid_ = uuid;
    username_ = std::move(username);
    properties_ = std::move(properties);
    authenticated_ = true;
}

const Uuid& Connection::uuid() const {
    return uuid_;
}

const std::string& Connection::username() const {
    return username_;
}

const std::vector<ProfileProperty>& Connection::properties() const {
    return properties_;
}

bool Connection::authenticated() const {
    return authenticated_;
}

void Connection::setEntityId(std::int32_t entityId) {
    entityId_ = entityId;
}

std::int32_t Connection::entityId() const {
    return entityId_;
}

void Connection::setGameMode(int mode) {
    gameMode_.store(mode, std::memory_order_relaxed);
}

int Connection::gameMode() const {
    return gameMode_.load(std::memory_order_relaxed);
}

double Connection::x() const {
    return x_.load(std::memory_order_relaxed);
}

double Connection::y() const {
    return y_.load(std::memory_order_relaxed);
}

double Connection::z() const {
    return z_.load(std::memory_order_relaxed);
}

float Connection::yaw() const {
    return yaw_.load(std::memory_order_relaxed);
}

float Connection::pitch() const {
    return pitch_.load(std::memory_order_relaxed);
}

bool Connection::onGround() const {
    return onGround_.load(std::memory_order_relaxed);
}

void Connection::setPosition(double x, double y, double z) {
    x_.store(x, std::memory_order_relaxed);
    y_.store(y, std::memory_order_relaxed);
    z_.store(z, std::memory_order_relaxed);
}

void Connection::setRotation(float yaw, float pitch) {
    yaw_.store(yaw, std::memory_order_relaxed);
    pitch_.store(std::clamp(pitch, -90.0F, 90.0F), std::memory_order_relaxed);
}

void Connection::setOnGround(bool value) {
    onGround_.store(value, std::memory_order_relaxed);
}

void Connection::setTeleportTarget(double x, double y, double z, float yaw, float pitch) {
    setPosition(x, y, z);
    setRotation(yaw, pitch);
}

int Connection::viewDistance() const {
    return viewDistance_.load(std::memory_order_relaxed);
}

void Connection::setViewDistance(int distance) {
    viewDistance_.store(std::max(2, distance), std::memory_order_relaxed);
}

int Connection::clientViewDistance() const {
    return clientViewDistance_;
}

void Connection::setClientViewDistance(int distance) {
    clientViewDistance_ = distance;
}

bool Connection::hasSentChunk(std::int64_t key) const {
    return sentChunks_.contains(key);
}

void Connection::markChunkSent(std::int64_t key) {
    sentChunks_.insert(key);
}

void Connection::clearSentChunks() {
    sentChunks_.clear();
}

std::chrono::steady_clock::time_point Connection::lastTimeUpdate() const {
    const auto value = lastTimeUpdateNs_.load(std::memory_order_relaxed);
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(value)));
}

void Connection::setLastTimeUpdate(std::chrono::steady_clock::time_point time) {
    lastTimeUpdateNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count(),
                            std::memory_order_relaxed);
}

float Connection::health() const {
    return health_.load(std::memory_order_relaxed);
}

void Connection::setHealth(float value) {
    health_.store(std::clamp(value, 0.0F, 20.0F), std::memory_order_relaxed);
}

int Connection::food() const {
    return food_.load(std::memory_order_relaxed);
}

void Connection::setFood(int value) {
    food_.store(std::clamp(value, 0, 20), std::memory_order_relaxed);
}

float Connection::saturation() const {
    return saturation_.load(std::memory_order_relaxed);
}

void Connection::setSaturation(float value) {
    saturation_.store(std::clamp(value, 0.0F, 20.0F), std::memory_order_relaxed);
}

bool Connection::playerDead() const {
    return playerDead_.load(std::memory_order_relaxed);
}

void Connection::setPlayerDead(bool value) {
    playerDead_.store(value, std::memory_order_relaxed);
}

void Connection::setFallHeight(double value) {
    fallHeight_ = value;
}

double Connection::fallHeight() const {
    return fallHeight_;
}

void Connection::resetSurvivalTimers() {
    const auto now = std::chrono::steady_clock::now();
    survivalTimers_ = SurvivalTimers{};
    survivalTimers_.lastLavaDamage = now;
    survivalTimers_.lastDrownDamage = now;
    survivalTimers_.lastHunger = now;
    survivalTimers_.lastRegen = now;
    survivalTimers_.lastStarve = now;
}

Connection::SurvivalTimers& Connection::survivalTimers() {
    return survivalTimers_;
}

ItemStack Connection::heldItem() const {
    std::lock_guard lock(inventoryMutex_);
    return inventory_[static_cast<std::size_t>(36 + heldSlot_.load(std::memory_order_relaxed))];
}

int Connection::heldSlot() const {
    return heldSlot_.load(std::memory_order_relaxed);
}

void Connection::setHeldSlot(int slot) {
    heldSlot_.store(std::clamp(slot, 0, 8), std::memory_order_relaxed);
}

std::array<ItemStack, 45> Connection::inventorySnapshot() const {
    std::lock_guard lock(inventoryMutex_);
    return inventory_;
}

ItemStack Connection::cursorItem() const {
    std::lock_guard lock(inventoryMutex_);
    return cursor_;
}

int Connection::addItem(const ItemStack& input) {
    const ItemStack stack = sanitizeItemStack(input);
    if (stack.empty()) {
        return 0;
    }
    std::vector<int> changed;
    int remaining = std::clamp<int>(stack.count, 0, 64 * 36);
    const int maxStack = maxStackSize(stack.id);
    {
        std::lock_guard lock(inventoryMutex_);
        for (int slot = 9; slot < 45 && remaining > 0; ++slot) {
            ItemStack& target = inventory_[static_cast<std::size_t>(slot)];
            if (target.empty() || target.id != stack.id || target.damage != stack.damage) {
                continue;
            }
            const int space = maxStack - target.count;
            if (space <= 0) {
                continue;
            }
            const int moved = std::min(space, remaining);
            target.count = static_cast<i8>(target.count + moved);
            remaining -= moved;
            changed.push_back(slot);
        }
        const auto fillRegion = [&](int begin, int end) {
            for (int slot = begin; slot < end && remaining > 0; ++slot) {
                ItemStack& target = inventory_[static_cast<std::size_t>(slot)];
                if (!target.empty()) {
                    continue;
                }
                const int moved = std::min(maxStack, remaining);
                target = ItemStack{stack.id, static_cast<i8>(moved), stack.damage};
                remaining -= moved;
                changed.push_back(slot);
            }
        };
        fillRegion(36, 45);
        fillRegion(9, 36);
    }
    for (const int slot : changed) {
        sendInventorySlot(slot);
    }
    return remaining;
}

ItemStack Connection::takeHeldItem(int amount) {
    return takeSlotItem(36 + heldSlot_.load(std::memory_order_relaxed), amount);
}

ItemStack Connection::takeSlotItem(int slot, int amount) {
    if (slot < 0 || slot >= 45) {
        return {};
    }
    ItemStack result;
    {
        std::lock_guard lock(inventoryMutex_);
        ItemStack& source = inventory_[static_cast<std::size_t>(slot)];
        if (source.empty()) {
            return {};
        }
        const int take = amount < 0 ? source.count : std::min(amount, static_cast<int>(source.count));
        result = sanitizeItemStack(ItemStack{source.id, static_cast<i8>(take), source.damage});
        source.count = static_cast<i8>(source.count - take);
        if (source.count <= 0) {
            source = ItemStack{};
        }
    }
    sendInventorySlot(slot);
    return result;
}

void Connection::setInventorySlot(int slot, const ItemStack& stack) {
    if (slot < 0 || slot >= 45) {
        return;
    }
    {
        std::lock_guard lock(inventoryMutex_);
        inventory_[static_cast<std::size_t>(slot)] = sanitizeItemStack(stack);
    }
    sendInventorySlot(slot);
}

void Connection::setCursorItem(const ItemStack& stack) {
    {
        std::lock_guard lock(inventoryMutex_);
        cursor_ = sanitizeItemStack(stack);
    }
    sendCursor();
}

void Connection::setInventoryFull(const std::array<ItemStack, 45>& inventory, const ItemStack& cursor) {
    std::lock_guard lock(inventoryMutex_);
    for (std::size_t i = 0; i < inventory_.size(); ++i) {
        inventory_[i] = sanitizeItemStack(inventory[i]);
    }
    cursor_ = sanitizeItemStack(cursor);
}

void Connection::clearInventory() {
    {
        std::lock_guard lock(inventoryMutex_);
        inventory_.fill(ItemStack{});
        cursor_ = ItemStack{};
    }
    sendInventoryState();
}

void Connection::sendInventoryState() {
    std::array<ItemStack, 45> inventory;
    ItemStack cursor;
    const int selected = heldSlot_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(inventoryMutex_);
        inventory = inventory_;
        cursor = cursor_;
    }
    send(0x30, [&](ByteBuffer& buffer) {
        buffer.writeU8(0);
        buffer.writeI16(45);
        for (const auto& stack : inventory) {
            writeItemStack(buffer, stack);
        }
    });
    send(0x09, [&](ByteBuffer& buffer) {
        buffer.writeI8(static_cast<i8>(selected));
    });
    send(0x2F, [&](ByteBuffer& buffer) {
        buffer.writeI8(-1);
        buffer.writeI16(-1);
        writeItemStack(buffer, cursor);
    });
}

void Connection::sendInventorySlot(int slot) {
    ItemStack stack;
    {
        std::lock_guard lock(inventoryMutex_);
        if (slot < 0 || slot >= 45) {
            return;
        }
        stack = inventory_[static_cast<std::size_t>(slot)];
    }
    send(0x2F, [&](ByteBuffer& buffer) {
        buffer.writeI8(0);
        buffer.writeI16(static_cast<i16>(slot));
        writeItemStack(buffer, stack);
    });
}

void Connection::sendCursor() {
    ItemStack cursor;
    {
        std::lock_guard lock(inventoryMutex_);
        cursor = cursor_;
    }
    send(0x2F, [&](ByteBuffer& buffer) {
        buffer.writeI8(-1);
        buffer.writeI16(-1);
        writeItemStack(buffer, cursor);
    });
}

void Connection::clickSlot(int slot, int button) {
    if (slot < 0 || slot >= 45 || (button != 0 && button != 1)) {
        return;
    }
    bool cursorChanged = false;
    bool slotChanged = false;
    {
        std::lock_guard lock(inventoryMutex_);
        ItemStack& slotStack = inventory_[static_cast<std::size_t>(slot)];
        ItemStack& cur = cursor_;
        const int maxStack = maxStackSize(slotStack.empty() ? cur.id : slotStack.id);
        if (button == 0) {
            if (cur.empty() && !slotStack.empty()) {
                cur = slotStack;
                slotStack = ItemStack{};
                cursorChanged = true;
                slotChanged = true;
            } else if (!cur.empty() && slotStack.empty()) {
                slotStack = cur;
                cur = ItemStack{};
                cursorChanged = true;
                slotChanged = true;
            } else if (!cur.empty() && !slotStack.empty()) {
                if (cur.id == slotStack.id && cur.damage == slotStack.damage && maxStack > 1) {
                    const int moved = std::min(maxStack - slotStack.count, static_cast<int>(cur.count));
                    if (moved > 0) {
                        slotStack.count = static_cast<i8>(slotStack.count + moved);
                        cur.count = static_cast<i8>(cur.count - moved);
                        if (cur.count <= 0) {
                            cur = ItemStack{};
                        }
                        cursorChanged = true;
                        slotChanged = true;
                    }
                } else {
                    std::swap(cur, slotStack);
                    cursorChanged = true;
                    slotChanged = true;
                }
            }
        } else {
            if (cur.empty() && !slotStack.empty()) {
                const int take = (slotStack.count + 1) / 2;
                cur = ItemStack{slotStack.id, static_cast<i8>(take), slotStack.damage};
                slotStack.count = static_cast<i8>(slotStack.count - take);
                if (slotStack.count <= 0) {
                    slotStack = ItemStack{};
                }
                cursorChanged = true;
                slotChanged = true;
            } else if (!cur.empty()) {
                if (slotStack.empty()) {
                    slotStack = ItemStack{cur.id, 1, cur.damage};
                    if (--cur.count <= 0) {
                        cur = ItemStack{};
                    }
                    cursorChanged = true;
                    slotChanged = true;
                } else if (slotStack.id == cur.id && slotStack.damage == cur.damage && slotStack.count < maxStack) {
                    ++slotStack.count;
                    if (--cur.count <= 0) {
                        cur = ItemStack{};
                    }
                    cursorChanged = true;
                    slotChanged = true;
                }
            }
        }
    }
    if (slotChanged) {
        sendInventorySlot(slot);
    }
    if (cursorChanged) {
        sendCursor();
    }
}

void Connection::shiftMove(int slot) {
    if (slot < 0 || slot >= 45) {
        return;
    }
    int targetBegin = 0;
    int targetEnd = 0;
    if (slot >= 9 && slot <= 35) {
        targetBegin = 36;
        targetEnd = 45;
    } else if (slot >= 36 && slot <= 44) {
        targetBegin = 9;
        targetEnd = 36;
    } else if (slot >= 5 && slot <= 8) {
        targetBegin = 9;
        targetEnd = 45;
    } else {
        return;
    }

    std::vector<int> changed;
    {
        std::lock_guard lock(inventoryMutex_);
        ItemStack moving = inventory_[static_cast<std::size_t>(slot)];
        if (moving.empty()) {
            return;
        }
        inventory_[static_cast<std::size_t>(slot)] = ItemStack{};
        const int maxStack = maxStackSize(moving.id);
        int remaining = moving.count;
        for (int target = targetBegin; target < targetEnd && remaining > 0; ++target) {
            ItemStack& stack = inventory_[static_cast<std::size_t>(target)];
            if (stack.empty() || stack.id != moving.id || stack.damage != moving.damage) {
                continue;
            }
            const int space = maxStack - stack.count;
            if (space <= 0) {
                continue;
            }
            const int moved = std::min(space, remaining);
            stack.count = static_cast<i8>(stack.count + moved);
            remaining -= moved;
            changed.push_back(target);
        }
        for (int target = targetBegin; target < targetEnd && remaining > 0; ++target) {
            ItemStack& stack = inventory_[static_cast<std::size_t>(target)];
            if (!stack.empty()) {
                continue;
            }
            const int moved = std::min(maxStack, remaining);
            stack = ItemStack{moving.id, static_cast<i8>(moved), moving.damage};
            remaining -= moved;
            changed.push_back(target);
        }
        if (remaining > 0) {
            inventory_[static_cast<std::size_t>(slot)] = ItemStack{moving.id, static_cast<i8>(remaining), moving.damage};
        }
        changed.push_back(slot);
    }
    for (const int changedSlot : changed) {
        sendInventorySlot(changedSlot);
    }
}

void Connection::swapHotbar(int slot, int hotbar) {
    if (slot < 9 || slot >= 45 || hotbar < 0 || hotbar > 8) {
        return;
    }
    const int hotbarSlot = 36 + hotbar;
    if (slot == hotbarSlot) {
        return;
    }
    {
        std::lock_guard lock(inventoryMutex_);
        std::swap(inventory_[static_cast<std::size_t>(slot)], inventory_[static_cast<std::size_t>(hotbarSlot)]);
    }
    sendInventorySlot(slot);
    sendInventorySlot(hotbarSlot);
}

void Connection::cloneToCursor(int slot) {
    if (slot < 0 || slot >= 45) {
        return;
    }
    {
        std::lock_guard lock(inventoryMutex_);
        cursor_ = inventory_[static_cast<std::size_t>(slot)];
    }
    sendCursor();
}

void Connection::acknowledgeKeepAlive(std::int32_t id) {
    if (awaitingKeepAlive_ && id == keepAliveId_) {
        awaitingKeepAlive_ = false;
    }
}

void Connection::setTimeoutMs(int timeoutMs) {
    timeoutMs_ = std::clamp(timeoutMs, 1000, 600000);
}

void Connection::setMaxOutputBuffer(std::size_t size) {
    maxOutputBuffer_ = std::max<std::size_t>(size, 65536);
}

bool Connection::hasBroadcastState() const {
    return broadcastStateValid_;
}

void Connection::setBroadcastState(double x, double y, double z, float yaw, float pitch) {
    broadcastX_ = x;
    broadcastY_ = y;
    broadcastZ_ = z;
    broadcastYaw_ = yaw;
    broadcastPitch_ = pitch;
    broadcastStateValid_ = true;
}

double Connection::broadcastX() const {
    return broadcastX_;
}

double Connection::broadcastY() const {
    return broadcastY_;
}

double Connection::broadcastZ() const {
    return broadcastZ_;
}

float Connection::broadcastYaw() const {
    return broadcastYaw_;
}

float Connection::broadcastPitch() const {
    return broadcastPitch_;
}

bool Connection::takeChatSlot(int intervalMs) {
    if (intervalMs <= 0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastChat_.time_since_epoch().count() != 0
        && now - lastChat_ < std::chrono::milliseconds(intervalMs)) {
        return false;
    }
    lastChat_ = now;
    return true;
}

bool Connection::allowPacket(int maxPerSecond) {
    if (maxPerSecond <= 0) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (packetWindowStart_.time_since_epoch().count() == 0
        || now - packetWindowStart_ >= std::chrono::seconds(1)) {
        packetWindowStart_ = now;
        packetCount_ = 0;
    }
    ++packetCount_;
    return packetCount_ <= maxPerSecond;
}

void Connection::setMaxPacketsPerSecond(int limit) {
    maxPacketsPerSecond_ = limit;
}

void Connection::sendLegacyPong(const std::string& text) {
    std::vector<u16> units;
    units.reserve(text.size());
    appendUtf16(units, text);
    if (units.size() > 32767) {
        units.resize(32767);
    }

    Bytes response;
    response.reserve(3 + units.size() * 2);
    response.push_back(0xFF);
    const auto length = static_cast<u16>(units.size() * 2);
    response.push_back(static_cast<u8>(length >> 8));
    response.push_back(static_cast<u8>(length & 0xFF));
    for (const u16 unit : units) {
        response.push_back(static_cast<u8>(unit >> 8));
        response.push_back(static_cast<u8>(unit & 0xFF));
    }

    {
        std::lock_guard lock(writeMutex_);
        if (dead()) {
            return;
        }
        output_.insert(output_.end(), response.begin(), response.end());
        setWriteInterestLocked(true);
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    closeRequestedNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), std::memory_order_relaxed);
    closeAfterFlush_.store(true, std::memory_order_release);
    loop_.wake();
}

bool Connection::takeMovementBroadcastSlot() {
    const auto now = std::chrono::steady_clock::now();
    if (lastMovementBroadcast_.time_since_epoch().count() != 0
        && now - lastMovementBroadcast_ < std::chrono::milliseconds(50)) {
        return false;
    }
    lastMovementBroadcast_ = now;
    return true;
}

const Bytes& Connection::verificationToken() const {
    return verificationToken_;
}

void Connection::setVerificationToken(Bytes token) {
    verificationToken_ = std::move(token);
}

const std::string& Connection::pendingUsername() const {
    return pendingUsername_;
}

void Connection::setPendingUsername(std::string username) {
    pendingUsername_ = std::move(username);
}

void Connection::closeSocket() {
    std::lock_guard lock(writeMutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Connection::onLoopThread() const {
    return loop_.isLoopThread();
}

void Connection::processInput() {
    const auto maxBuffered = maxPacketSize_;
    while (!dead()) {
        if (state() == ConnectionState::Handshake && protocolVersion_ == 0 && input_.remaining() > 0
            && input_.view().front() == 0xFE) {
            input_.skip(input_.remaining());
            handler_.onLegacyPing(*this);
            return;
        }

        const std::size_t start = input_.readPosition();
        i32 length = 0;
        try {
            length = input_.readVarInt();
        } catch (const BufferUnderflow&) {
            input_.setReadPosition(start);
            break;
        } catch (const std::exception& error) {
            Log::warn("protocol error from {}: {}", remoteAddress_, error.what());
            fail(std::string("Protocol error: ") + error.what());
            break;
        }
        if (length <= 0 || static_cast<std::size_t>(length) > maxBuffered) {
            fail("Invalid packet length");
            break;
        }
        if (input_.remaining() < static_cast<std::size_t>(length)) {
            input_.setReadPosition(start);
            break;
        }

        ByteBuffer frame(input_.readBytes(static_cast<std::size_t>(length)));
        try {
            ByteBuffer body;
            if (compressionEnabled_.load(std::memory_order_acquire)) {
                const i32 uncompressed = frame.readVarInt();
                if (uncompressed != 0) {
                    if (uncompressed < 0 || static_cast<std::size_t>(uncompressed) > maxBuffered) {
                        throw ProtocolError("invalid uncompressed size");
                    }
                    const Bytes compressed = frame.readRemaining();
                    Bytes raw = zlibDecompress(compressed, static_cast<std::size_t>(uncompressed), maxBuffered);
                    body = ByteBuffer(std::move(raw));
                } else {
                    body = std::move(frame);
                }
            } else {
                body = std::move(frame);
            }
            const i32 packetId = body.readVarInt();
            if (!allowPacket(maxPacketsPerSecond_)) {
                fail("Too many packets");
                break;
            }
            handler_.onPacket(*this, packetId, body);
        } catch (const std::exception& error) {
            Log::warn("protocol error from {}: {}", remoteAddress_, error.what());
            fail(std::string("Protocol error: ") + error.what());
            break;
        }
    }
    input_.compact();
}

void Connection::fail(const std::string& reason) {
    if (dead()) {
        return;
    }
    const auto current = state();
    if (current == ConnectionState::Play || current == ConnectionState::Login) {
        disconnect(chat::text(reason));
    } else {
        requestClose();
    }
}

void Connection::flushLocked() {
    if (fd_ < 0 || dead()) {
        return;
    }
    while (outputOffset_ < output_.size()) {
        const std::size_t remaining = output_.size() - outputOffset_;
        const ssize_t sent = ::send(fd_, output_.data() + outputOffset_, remaining, MSG_NOSIGNAL);
        if (sent > 0) {
            outputOffset_ += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            setWriteInterestLocked(true);
            break;
        }
        dead_.store(true, std::memory_order_release);
        return;
    }

    if (outputOffset_ >= output_.size()) {
        output_.clear();
        outputOffset_ = 0;
        setWriteInterestLocked(false);
        return;
    }

    if (outputOffset_ > 1024 * 1024) {
        output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(outputOffset_));
        outputOffset_ = 0;
    }
}

void Connection::setWriteInterestLocked(bool enabled) {
    if (fd_ < 0 || writeInterest_ == enabled) {
        return;
    }
    writeInterest_ = enabled;
    loop_.setWriteInterest(fd_, enabled);
}

}
