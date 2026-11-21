#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/Types.hpp"
#include "common/Uuid.hpp"
#include "net/ByteBuffer.hpp"
#include "protocol/ItemStack.hpp"
#include "protocol/Profile.hpp"

namespace mc {

class EventLoop;
class ConnectionHandler;
class Encryption;

enum class ConnectionState {
    Handshake,
    Status,
    Login,
    Play,
    Closed
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd, EventLoop& loop, ConnectionHandler& handler, std::string remoteAddress, std::size_t maxPacketSize);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int fd() const;
    EventLoop& loop();
    const std::string& remoteAddress() const;

    ConnectionState state() const;
    void setState(ConnectionState state);

    bool dead() const;
    bool closeAfterFlush() const;
    bool outEmpty() const;
    void requestClose();
    void closeGracefully();
    void disconnect(const std::string& jsonReason);

    void onReadable();
    void onWritable();
    void tick(std::chrono::steady_clock::time_point now);

    void sendPacket(ByteBuffer body);

    template <typename Writer>
    void send(std::int32_t packetId, Writer&& writer) {
        ByteBuffer body;
        body.writeVarInt(packetId);
        writer(body);
        sendPacket(std::move(body));
    }

    void enableCompression(int threshold);
    void enableEncryption(std::span<const u8> sharedSecret);

    void setProtocolVersion(std::int32_t version);
    std::int32_t protocolVersion() const;
    void setVirtualHost(std::string host);
    const std::string& virtualHost() const;

    void setIdentity(const Uuid& uuid, std::string username, std::vector<ProfileProperty> properties);
    const Uuid& uuid() const;
    const std::string& username() const;
    const std::vector<ProfileProperty>& properties() const;
    bool authenticated() const;

    void setEntityId(std::int32_t entityId);
    std::int32_t entityId() const;

    void setGameMode(int mode);
    int gameMode() const;

    double x() const;
    double y() const;
    double z() const;
    float yaw() const;
    float pitch() const;
    bool onGround() const;
    void setPosition(double x, double y, double z);
    void setRotation(float yaw, float pitch);
    void setOnGround(bool value);
    void setTeleportTarget(double x, double y, double z, float yaw, float pitch);

    int viewDistance() const;
    void setViewDistance(int distance);
    int clientViewDistance() const;
    void setClientViewDistance(int distance);

    bool hasSentChunk(std::int64_t key) const;
    void markChunkSent(std::int64_t key);
    void clearSentChunks();

    std::chrono::steady_clock::time_point lastTimeUpdate() const;
    void setLastTimeUpdate(std::chrono::steady_clock::time_point time);

    float health() const;
    void setHealth(float value);
    int food() const;
    void setFood(int value);
    float saturation() const;
    void setSaturation(float value);
    bool playerDead() const;
    void setPlayerDead(bool value);
    void setFallHeight(double value);
    double fallHeight() const;
    void resetSurvivalTimers();

    struct SurvivalTimers {
        std::chrono::steady_clock::time_point lastLavaDamage{};
        std::chrono::steady_clock::time_point lastDrownDamage{};
        std::chrono::steady_clock::time_point lastHunger{};
        std::chrono::steady_clock::time_point lastRegen{};
        std::chrono::steady_clock::time_point lastStarve{};
        int waterTicks{0};
    };

    SurvivalTimers& survivalTimers();

    ItemStack heldItem() const;
    int heldSlot() const;
    void setHeldSlot(int slot);
    std::array<ItemStack, 45> inventorySnapshot() const;
    ItemStack cursorItem() const;
    int addItem(const ItemStack& stack);
    ItemStack takeHeldItem(int amount);
    ItemStack takeSlotItem(int slot, int amount);
    void setInventorySlot(int slot, const ItemStack& stack);
    void setCursorItem(const ItemStack& stack);
    void setInventoryFull(const std::array<ItemStack, 45>& inventory, const ItemStack& cursor);
    void clearInventory();
    void sendInventoryState();
    void sendInventorySlot(int slot);
    void sendCursor();
    void clickSlot(int slot, int button);
    void shiftMove(int slot);
    void swapHotbar(int slot, int hotbar);
    void cloneToCursor(int slot);

    void acknowledgeKeepAlive(std::int32_t id);
    void setTimeoutMs(int timeoutMs);
    bool takeMovementBroadcastSlot();
    bool hasBroadcastState() const;
    void setBroadcastState(double x, double y, double z, float yaw, float pitch);
    double broadcastX() const;
    double broadcastY() const;
    double broadcastZ() const;
    float broadcastYaw() const;
    float broadcastPitch() const;
    bool takeChatSlot(int intervalMs);
    bool allowPacket(int maxPerSecond);
    void setMaxPacketsPerSecond(int limit);
    void sendLegacyPong(const std::string& text);

    const Bytes& verificationToken() const;
    void setVerificationToken(Bytes token);
    const std::string& pendingUsername() const;
    void setPendingUsername(std::string username);

    void closeSocket();

private:
    bool onLoopThread() const;
    void processInput();
    void fail(const std::string& reason);
    void flushLocked();
    void setWriteInterestLocked(bool enabled);

    int fd_;
    EventLoop& loop_;
    ConnectionHandler& handler_;
    std::string remoteAddress_;
    std::size_t maxPacketSize_;

    std::atomic<ConnectionState> state_{ConnectionState::Handshake};
    std::atomic<bool> dead_{false};
    std::atomic<bool> closeAfterFlush_{false};
    std::int32_t protocolVersion_{0};
    std::string virtualHost_;

    ByteBuffer input_;
    mutable std::mutex writeMutex_;
    std::vector<u8> output_;
    std::size_t outputOffset_{0};
    bool writeInterest_{false};
    std::unique_ptr<Encryption> encryption_;
    std::atomic<bool> compressionEnabled_{false};
    std::atomic<int> compressionThreshold_{256};

    std::chrono::steady_clock::time_point connectedAt_;
    std::chrono::steady_clock::time_point lastRead_;
    std::chrono::steady_clock::time_point lastKeepAliveSent_;
    std::atomic<std::int64_t> closeRequestedNs_{0};
    bool awaitingKeepAlive_{false};
    std::int32_t keepAliveId_{0};
    int timeoutMs_{30000};

    Uuid uuid_;
    std::string username_;
    std::vector<ProfileProperty> properties_;
    bool authenticated_{false};
    std::int32_t entityId_{0};
    std::atomic<int> gameMode_{0};

    std::atomic<double> x_{0.5};
    std::atomic<double> y_{4.0};
    std::atomic<double> z_{0.5};
    std::atomic<float> yaw_{0.0F};
    std::atomic<float> pitch_{0.0F};
    std::atomic<bool> onGround_{true};
    std::atomic<int> viewDistance_{4};
    int clientViewDistance_{4};
    std::unordered_set<std::int64_t> sentChunks_;
    std::atomic<std::int64_t> lastTimeUpdateNs_{0};
    std::chrono::steady_clock::time_point lastMovementBroadcast_;
    std::chrono::steady_clock::time_point lastChat_;
    bool broadcastStateValid_{false};
    double broadcastX_{0.0};
    double broadcastY_{0.0};
    double broadcastZ_{0.0};
    float broadcastYaw_{0.0F};
    float broadcastPitch_{0.0F};
    std::chrono::steady_clock::time_point packetWindowStart_;
    int packetCount_{0};
    int maxPacketsPerSecond_{2000};

    Bytes verificationToken_;
    std::string pendingUsername_;

    mutable std::mutex inventoryMutex_;
    std::array<ItemStack, 45> inventory_{};
    ItemStack cursor_;
    std::atomic<int> heldSlot_{0};
    std::atomic<float> health_{20.0F};
    std::atomic<int> food_{20};
    std::atomic<float> saturation_{5.0F};
    std::atomic<bool> playerDead_{false};
    double fallHeight_{4.0};
    SurvivalTimers survivalTimers_{};
};

}
