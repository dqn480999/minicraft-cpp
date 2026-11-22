#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mc {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    std::uint16_t port{25565};
    bool onlineMode{false};
    bool onlineModeIpCheck{false};
    bool nameReplace{true};
    int maxPlayers{20};
    std::string motd{"A Minicraft C++ Server"};
    int viewDistance{6};
    int difficulty{1};
    int gameMode{0};
    bool hardcore{false};
    int compressionThreshold{256};
    int ioThreads{0};
    bool pinIoThreads{false};
    int playerTimeoutMs{30000};
    std::size_t maxPacketSize{2097152};
    std::size_t maxOutputBuffer{8 * 1024 * 1024};
    int maxItemEntities{2048};
    int maxFallingBlocks{1024};
    int maxMobs{256};
    std::string levelType{"flat"};
    std::uint64_t levelSeed{0};
    std::string ops;
    std::string opsFile{"ops.txt"};
    int maxCachedChunks{1024};
    bool playerData{true};
    std::string playerDataDir{"playerdata"};
    bool worldSave{true};
    std::string worldDir{"world"};
    bool itemDrops{true};
    bool starterKit{true};
    int itemDespawnSeconds{300};
    int itemPickupDelayMs{3000};
    double itemPickupRadius{1.5};
    bool itemPickupAnimation{false};
    bool sandPhysics{true};
    bool fluidsEnabled{true};
    int fluidUpdatesPerTick{32};
    int fluidTickInterval{4};
    int hungerIntervalSeconds{20};
    double timeScale{0.5};
    int autosaveSeconds{300};
    int chatRateLimitMs{500};
    int maxPacketsPerSecond{2000};
    bool legacyPing{true};
    bool scriptsEnabled{true};
    std::string scriptDir{"scripts"};
    int scriptTickIntervalMs{1000};
    int scriptMaxInstructions{200000};
    bool scriptTrace{false};
    bool scriptDisassemble{false};
    bool modsEnabled{true};
    std::string modDir{"mods"};
    bool debug{false};
    bool color{true};
    std::string listenerName{"minicraft"};
    std::string logFile{"server.log"};
};

class Config {
public:
    static ServerConfig load(const std::string& path);
    static ServerConfig defaults();
    static bool applyLine(ServerConfig& config, std::string_view line);
};

}
