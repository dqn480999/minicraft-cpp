#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/Config.hpp"
#include "common/ThreadPool.hpp"
#include "lang/ScriptEngine.hpp"
#include "mod/ModManager.hpp"
#include "net/ConnectionHandler.hpp"
#include "net/TcpServer.hpp"
#include "protocol/ItemStack.hpp"
#include "protocol/Profile.hpp"
#include "server/CommandManager.hpp"
#include "server/PlayerManager.hpp"
#include "world/World.hpp"

namespace mc {

class Connection;
class ByteBuffer;
class RsaKey;

class Server final : public ConnectionHandler {
public:
    explicit Server(ServerConfig config);
    ~Server() override;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void run();
    void stop();
    void requestStop();

    void onOpen(Connection& connection) override;
    void onPacket(Connection& connection, std::int32_t packetId, ByteBuffer& payload) override;
    void onTick(Connection& connection) override;
    void onClose(Connection& connection) override;
    void onLegacyPing(Connection& connection) override;

    const ServerConfig& config() const;
    World& world();
    PlayerManager& players();
    ScriptEngine& scripts();
    bool reloadScripts();
    ModManager& mods();

    void sendSystem(Connection& connection, const std::string& message);
    void sendChat(Connection& connection, const std::string& message, std::int8_t position);
    void broadcastSystem(const std::string& message);
    void broadcastChat(const std::string& message);
    void broadcastPlayerChat(Connection& source, const std::string& message);
    void broadcastTime();
    void setGameMode(const std::shared_ptr<Connection>& connection, int mode);
    void teleport(const std::shared_ptr<Connection>& connection, double x, double y, double z);
    void kick(const std::shared_ptr<Connection>& connection, const std::string& reason);

    bool isOp(std::string_view name) const;
    void setOp(std::string_view name, bool value);
    void saveAllPlayers();
    void cleanupItems();
    void clearPlayerInventory(const std::shared_ptr<Connection>& connection);
    void setBlockState(std::int32_t x, std::int32_t y, std::int32_t z, u16 blockState);
    void tryGrowGrassAt(std::int32_t x, std::int32_t y, std::int32_t z);
    void healPlayer(Connection& connection, float amount);
    void feedPlayer(Connection& connection, int amount);
    void hurtPlayer(Connection& connection, float amount);
    void setFoodLevel(Connection& connection, int value);
    void summonMob(u8 type, double x, double y, double z);
    std::int64_t uptimeSeconds() const;

private:
    struct FallingBlock {
        std::int32_t entityId{0};
        u16 blockState{0};
        std::int32_t blockX{0};
        std::int32_t blockZ{0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        double velocityY{0.0};
        double lastX{0.0};
        double lastY{0.0};
        double lastZ{0.0};
    };

    struct MobEntity {
        std::int32_t entityId{0};
        u8 type{0};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        float yaw{0.0F};
        double velocityY{0.0};
        double velocityX{0.0};
        double velocityZ{0.0};
        double lastX{0.0};
        double lastY{0.0};
        double lastZ{0.0};
        float lastYaw{0.0F};
        float health{10.0F};
        bool onGround{true};
        int wanderTicks{0};
    };

    bool consumeFood(Connection& connection, const ItemStack& stack);
    void useBucket(Connection& connection, const ItemStack& stack, std::int32_t x, std::int32_t y, std::int32_t z, i8 face);
    void spawnMob(u8 type, double x, double y, double z, float yaw);
    void updateMobs();
    void sendMobSpawn(Connection& connection, const MobEntity& mob);
    void sendMobMovement(const MobEntity& mob);
    void sendActiveMobs(Connection& connection);
    void attackMob(Connection& connection, std::int32_t entityId);
    void kickExistingByName(std::string_view name);
    void generateVillage();
    void spawnInitialMobs();
    struct ItemEntity {
        std::int32_t entityId{0};
        u16 itemId{0};
        u16 damage{0};
        i8 count{1};
        double x{0.0};
        double y{0.0};
        double z{0.0};
        double velocityX{0.0};
        double velocityY{0.0};
        double velocityZ{0.0};
        bool resting{false};
        double lastX{0.0};
        double lastY{0.0};
        double lastZ{0.0};
        std::chrono::steady_clock::time_point spawned{};
        std::chrono::steady_clock::time_point pickupAt{};
    };

    void handleHandshake(Connection& connection, std::int32_t packetId, ByteBuffer& payload);
    void handleStatus(Connection& connection, std::int32_t packetId, ByteBuffer& payload);
    void handleLogin(Connection& connection, std::int32_t packetId, ByteBuffer& payload);
    void handlePlay(Connection& connection, std::int32_t packetId, ByteBuffer& payload);

    void sendStatus(Connection& connection);
    void finishLogin(const std::shared_ptr<Connection>& connection, const Profile& profile);
    void sendJoinSequence(Connection& connection);
    void sendInitialPlayerList(Connection& connection);
    void sendPlayerInfoAdd(Connection& connection, const std::vector<std::shared_ptr<Connection>>& entries);
    void sendPlayerInfoAddOne(Connection& connection, const std::shared_ptr<Connection>& entry);
    void sendPlayerInfoRemove(Connection& connection, const Uuid& uuid);
    void sendPlayerInfoGameMode(Connection& connection, const std::shared_ptr<Connection>& entry);
    void sendChunksAround(Connection& connection);
    void sendChunkTo(Connection& connection, std::int32_t chunkX, std::int32_t chunkZ);
    void refreshChunkAt(std::int32_t worldX, std::int32_t worldZ);
    void sendAbilities(Connection& connection);
    void sendRespawn(Connection& connection);
    void sendHealth(Connection& connection);
    void applyDamage(Connection& connection, float amount);
    void handleRespawn(Connection& connection);
    void updateSurvival(Connection& connection);
    void sendTime(Connection& connection);
    void sendPlayerSpawn(Connection& viewer, const std::shared_ptr<Connection>& subject);
    void broadcastPlayerSpawns(Connection& joining);
    void broadcastEntityTeleport(const std::shared_ptr<Connection>& subject);
    void broadcastEntityMovement(const std::shared_ptr<Connection>& subject);
    void broadcastEntityDestroy(std::int32_t entityId);
    void broadcastBlockChange(std::int32_t x, std::int32_t y, std::int32_t z, u16 block);
    void sendJoinMessage(Connection& connection);
    void sendLeaveMessage(const std::string& username);
    void loadOps();
    void saveOps();
    void tickLoop();
    void updateItems();
    void spawnItemDrop(u16 blockState, std::int32_t x, std::int32_t y, std::int32_t z);
    void sendItemEntity(Connection& connection, const ItemEntity& item);
    void sendActiveItems(Connection& connection);
    void sendItemMovement(const ItemEntity& item);
    void sendItemMetadata(Connection& connection, const ItemEntity& item);
    void sendItemMetadata(const ItemEntity& item);
    void sendItemDestroy(std::int32_t entityId);
    void spawnItemEntity(const ItemStack& stack, double x, double y, double z, double velocityX, double velocityY, double velocityZ);
    void tryStartFalling(std::int32_t x, std::int32_t y, std::int32_t z);
    void scheduleFluidUpdate(std::int32_t x, std::int32_t y, std::int32_t z);
    void scheduleFluidNeighbors(std::int32_t x, std::int32_t y, std::int32_t z);
    void updateFluids();
    void updateFallingBlocks();
    void sendFallingSpawn(Connection& connection, const FallingBlock& block);
    void sendFallingMovement(const FallingBlock& block);
    void sendActiveFallingBlocks(Connection& connection);
    void dropItem(Connection& connection, const ItemStack& stack);
    void dropCursor(Connection& connection, bool dropAll);
    void giveStarterKit(Connection& connection);
    void sendEquipment(Connection& viewer, const std::shared_ptr<Connection>& subject);
    void broadcastEquipment(const std::shared_ptr<Connection>& subject);
    std::string playerDataPath(const Uuid& uuid) const;
    bool loadPlayerData(Connection& connection);
    void savePlayerData(Connection& connection);

    int abilitiesFlags(int mode) const;

    ServerConfig config_;
    PlayerManager players_;
    World world_;
    CommandManager commands_;
    ThreadPool authPool_;
    TcpServer net_;
    std::unique_ptr<RsaKey> rsaKey_;
    ScriptEngine scriptEngine_;
    ModManager mods_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<std::int32_t> nextEntityId_{1};
    std::chrono::steady_clock::time_point startedAt_{};
    mutable std::shared_mutex opsMutex_;
    std::unordered_set<std::string> ops_;
    std::mutex playerDataMutex_;
    std::mutex itemsMutex_;
    std::vector<ItemEntity> items_;
    std::mutex fallingMutex_;
    std::mutex fallingStartMutex_;
    std::vector<FallingBlock> fallingBlocks_;
    std::mutex fluidMutex_;
    std::unordered_set<u64> fluidUpdates_;
    std::uint64_t fluidTickCounter_{0};
    std::mutex mobsMutex_;
    std::vector<MobEntity> mobs_;
    std::mt19937_64 mobRng_;
    std::thread tickThread_;
    std::atomic<bool> tickRunning_{false};
    std::condition_variable tickCondition_;
    std::mutex tickMutex_;
    std::chrono::steady_clock::time_point nextAutosave_{};
};

}
