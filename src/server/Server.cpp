#include "server/Server.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <pthread.h>
#include <span>
#include <thread>
#include <utility>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "common/Crypto.hpp"
#include "common/Json.hpp"
#include "common/Log.hpp"
#include "common/Uuid.hpp"
#include "net/ByteBuffer.hpp"
#include "net/Connection.hpp"
#include "net/EventLoop.hpp"
#include "protocol/Auth.hpp"
#include "protocol/Chat.hpp"
#include "world/Blocks.hpp"

namespace mc {
namespace {

volatile std::sig_atomic_t g_signalRequested = 0;

void handleSignal(int) {
    g_signalRequested = 1;
}

std::int32_t floorDiv(std::int32_t value, std::int32_t divisor) {
    std::int32_t quotient = value / divisor;
    if (value % divisor < 0) {
        --quotient;
    }
    return quotient;
}

bool validUsername(std::string_view name) {
    if (name.empty() || name.size() > 16) {
        return false;
    }
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) == 0 && c != '_') {
            return false;
        }
    }
    return true;
}

bool validMovement(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
        && std::abs(x) <= 30000000.0
        && std::abs(z) <= 30000000.0
        && y >= -256.0
        && y <= 2048.0;
}

u64 encodeFluidKey(i32 x, i32 y, i32 z) {
    return (static_cast<u64>(static_cast<u32>(x) & 0x3FFFFFF) << 38)
        | (static_cast<u64>(static_cast<u32>(y) & 0xFFF) << 26)
        | static_cast<u64>(static_cast<u32>(z) & 0x3FFFFFF);
}

void decodeFluidKey(u64 key, i32& x, i32& y, i32& z) {
    x = static_cast<i32>((key >> 38) & 0x3FFFFFF);
    if ((x & 0x2000000) != 0) {
        x -= 0x4000000;
    }
    y = static_cast<i32>((key >> 26) & 0xFFF);
    z = static_cast<i32>(key & 0x3FFFFFF);
    if ((z & 0x2000000) != 0) {
        z -= 0x4000000;
    }
}

bool itemToBlock(u16 itemId, u16 damage, u16& block);

u16 resolvePlacementState(u16 itemId, u16 damage, i8 face, float yaw) {
    if (itemId == 50 || itemId == 76) {
        if (face == 0) {
            return 0;
        }
        u16 meta = 5;
        if (face == 1) {
            meta = 5;
        } else if (face == 2) {
            meta = 4;
        } else if (face == 3) {
            meta = 3;
        } else if (face == 4) {
            meta = 2;
        } else {
            meta = 1;
        }
        return static_cast<u16>((itemId << 4) | meta);
    }

    if (itemId == 44 || itemId == 126 || itemId == 182) {
        u16 meta = damage & 0x07;
        if (face == 0) {
            meta |= 0x08;
        }
        return static_cast<u16>((itemId << 4) | meta);
    }

    if (itemId == 17 || itemId == 162) {
        u16 meta = damage & (itemId == 17 ? 0x03 : 0x01);
        if (face == 4 || face == 5) {
            meta |= 0x04;
        } else if (face == 2 || face == 3) {
            meta |= 0x08;
        }
        return static_cast<u16>((itemId << 4) | meta);
    }

    switch (itemId) {
    case 53:
    case 67:
    case 108:
    case 109:
    case 114:
    case 128:
    case 134:
    case 135:
    case 136:
    case 156:
    case 163:
    case 164:
    case 180: {
        const int index = static_cast<int>(std::floor((yaw * 4.0F / 360.0F) + 0.5F)) & 3;
        u16 meta = 0;
        if (index == 0) {
            meta = 2;
        } else if (index == 1) {
            meta = 1;
        } else if (index == 2) {
            meta = 3;
        }
        if (face == 0) {
            meta |= 0x04;
        }
        return static_cast<u16>((itemId << 4) | meta);
    }
    default:
        break;
    }

    u16 block = kAir;
    if (!itemToBlock(itemId, damage, block)) {
        return 0;
    }
    return block;
}

std::string lowerName(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

bool itemToBlock(u16 itemId, u16 damage, u16& block) {
    if (itemId == 0 || itemId > 198) {
        return false;
    }
    switch (itemId) {
    case 8:
    case 9:
    case 10:
    case 11:
    case 36:
    case 51:
    case 90:
    case 119:
    case 120:
        return false;
    default:
        break;
    }
    block = static_cast<u16>((itemId << 4) | (damage & 0x0F));
    return true;
}

ItemStack resolveBlockDrop(u16 blockState, i32 x, i32 y, i32 z) {
    const u16 blockId = static_cast<u16>(blockState >> 4);
    const u16 meta = static_cast<u16>(blockState & 0x0F);
    const u32 hash = static_cast<u32>(x) * 73856093U
        ^ static_cast<u32>(y) * 19349663U
        ^ static_cast<u32>(z) * 83492791U;
    const auto make = [](i16 id, i8 count, u16 damage) {
        return sanitizeItemStack(ItemStack{id, count, static_cast<i16>(damage)});
    };

    switch (blockId) {
    case 1:
        if (meta == 0) {
            return make(4, 1, 0);
        }
        return make(1, 1, meta);
    case 2:
        return make(3, 1, 0);
    case 3:
        return make(3, 1, meta & 0x03);
    case 5:
        return make(5, 1, meta & 0x07);
    case 6:
        return make(6, 1, meta & 0x07);
    case 12:
        return make(12, 1, meta & 0x01);
    case 13:
        if (hash % 10U == 0U) {
            return make(318, 1, 0);
        }
        return make(13, 1, 0);
    case 16:
        return make(263, 1, 0);
    case 17:
        return make(17, 1, meta & 0x03);
    case 18:
    case 161: {
        if (hash % 20U == 0U) {
            const u16 saplingMeta = blockId == 18 ? meta & 0x03 : static_cast<u16>((meta & 0x03) + 4);
            return make(6, 1, saplingMeta);
        }
        if (hash % 200U == 1U) {
            return make(260, 1, 0);
        }
        return ItemStack{};
    }
    case 21:
        return make(351, static_cast<i8>(4 + hash % 5U), 4);
    case 24:
        return make(24, 1, meta & 0x03);
    case 26:
        return make(355, 1, 0);
    case 29:
        return make(29, 1, 0);
    case 31:
        if (hash % 8U == 0U) {
            return make(295, 1, 0);
        }
        return ItemStack{};
    case 33:
        return make(33, 1, 0);
    case 20:
    case 34:
    case 36:
    case 51:
    case 52:
    case 79:
    case 90:
    case 95:
    case 102:
    case 119:
    case 120:
    case 160:
    case 166:
    case 174:
        return ItemStack{};
    case 35:
        return make(35, 1, meta & 0x0F);
    case 38:
    case 39:
    case 40:
        return make(static_cast<i16>(blockId), 1, 0);
    case 43:
        return make(44, 2, meta & 0x07);
    case 44:
        return make(44, 1, meta & 0x07);
    case 50:
        return make(50, 1, 0);
    case 53:
    case 67:
    case 108:
    case 109:
    case 114:
    case 128:
    case 134:
    case 135:
    case 136:
    case 156:
    case 163:
    case 164:
    case 180:
        return make(static_cast<i16>(blockId), 1, 0);
    case 55:
        return make(331, 1, 0);
    case 59:
        if (meta >= 7) {
            return make(296, 1, 0);
        }
        return make(295, 1, 0);
    case 60:
    case 110:
        return make(3, 1, 0);
    case 62:
        return make(61, 1, 0);
    case 63:
    case 68:
        return make(323, 1, 0);
    case 64:
    case 193:
    case 194:
    case 195:
    case 196:
    case 197: {
        static constexpr i16 doorItems[6] = {324, 427, 428, 429, 430, 431};
        const std::size_t index = blockId == 64 ? 0 : static_cast<std::size_t>(blockId - 192);
        return make(doorItems[index], 1, 0);
    }
    case 71:
        return make(330, 1, 0);
    case 73:
    case 74:
        return make(331, static_cast<i8>(4 + hash % 2U), 0);
    case 75:
    case 76:
        return make(76, 1, 0);
    case 78:
        return make(332, static_cast<i8>(2 + hash % 8U), 0);
    case 80:
        return make(332, 4, 0);
    case 81:
        return make(81, 1, 0);
    case 82:
        return make(337, 4, 0);
    case 83:
        return make(338, 1, 0);
    case 84:
        return make(84, 1, 0);
    case 85:
    case 101:
    case 106:
    case 107:
    case 111:
    case 112:
    case 113:
    case 116:
    case 121:
    case 122:
    case 130:
    case 131:
    case 133:
    case 137:
    case 138:
    case 139:
    case 143:
    case 147:
    case 148:
    case 151:
    case 152:
    case 154:
    case 157:
    case 158:
    case 165:
    case 167:
    case 169:
    case 170:
    case 172:
    case 173:
    case 183:
    case 184:
    case 185:
    case 186:
    case 187:
    case 188:
    case 189:
    case 190:
    case 191:
    case 192:
        return make(static_cast<i16>(blockId), 1, 0);
    case 86:
    case 91:
        return make(86, 1, 0);
    case 89:
        return make(348, static_cast<i8>(2 + hash % 3U), 0);
    case 93:
    case 94:
        return make(356, 1, 0);
    case 96:
        return make(96, 1, 0);
    case 97:
        return make(97, 1, 0);
    case 98:
        return make(98, 1, meta & 0x03);
    case 99:
        return make(39, 1, 0);
    case 100:
        return make(40, 1, 0);
    case 103:
        return make(360, static_cast<i8>(3 + hash % 5U), 0);
    case 104:
    case 105:
        return ItemStack{};
    case 115:
        return make(372, static_cast<i8>(1 + hash % 3U), 0);
    case 117:
        return make(379, 1, 0);
    case 118:
        return make(380, 1, 0);
    case 123:
    case 124:
        return make(123, 1, 0);
    case 125:
        return make(126, 2, meta & 0x07);
    case 126:
        return make(126, 1, meta & 0x07);
    case 127:
        return make(351, 3, 3);
    case 129:
        return make(388, 1, 0);
    case 132:
        return make(287, 1, 0);
    case 140:
        return make(390, 1, 0);
    case 141:
        return make(391, static_cast<i8>(1 + hash % 4U), 0);
    case 142:
        return make(392, static_cast<i8>(1 + hash % 4U), 0);
    case 144:
        return make(397, 1, 0);
    case 145:
        return make(145, 1, meta & 0x03);
    case 146:
        return make(146, 1, 0);
    case 149:
    case 150:
        return make(404, 1, 0);
    case 153:
        return make(406, 1, 0);
    case 155: {
        const u16 damage = meta >= 2 ? 2 : meta;
        return make(155, 1, damage);
    }
    case 159:
        return make(159, 1, meta & 0x0F);
    case 162:
        return make(162, 1, meta & 0x03);
    case 168:
        return make(168, 1, meta & 0x03);
    case 171:
        return make(171, 1, meta & 0x0F);
    case 175:
        return make(175, 1, meta & 0x07);
    case 176:
    case 177:
        return make(425, 1, meta & 0x0F);
    case 178:
        return make(151, 1, 0);
    case 179:
        return make(179, 1, meta & 0x03);
    case 181:
        return make(182, 2, 0);
    case 182:
        return make(182, 1, 0);
    default:
        if (isValidItemId(static_cast<i16>(blockId))) {
            return make(static_cast<i16>(blockId), 1, 0);
        }
        return ItemStack{};
    }
}

i8 angleByte(float angle) {
    const float scaled = angle * (256.0F / 360.0F);
    const auto rounded = static_cast<i32>(std::lround(scaled));
    return static_cast<i8>(static_cast<u8>(rounded));
}

i32 fixedPoint(double value) {
    const double scaled = std::floor(value * 32.0);
    const double clamped = std::clamp(scaled, -2147483000.0, 2147483000.0);
    return static_cast<i32>(clamped);
}

std::string gameModeName(int mode) {
    switch (mode) {
    case 0:
        return "survival";
    case 1:
        return "creative";
    case 2:
        return "adventure";
    case 3:
        return "spectator";
    default:
        return "survival";
    }
}

}

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      world_(config_.levelSeed, config_.difficulty, static_cast<std::size_t>(config_.maxCachedChunks),
             config_.worldDir, config_.worldSave, config_.timeScale),
      commands_(*this),
      authPool_(2),
      net_(*this, config_),
      scriptEngine_(*this) {}

Server::~Server() {
    stop();
}

bool Server::start() {
    if (running_.exchange(true)) {
        return true;
    }
    if (stopped_.load()) {
        running_.store(false);
        return false;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    startedAt_ = std::chrono::steady_clock::now();
    loadOps();

    if (config_.onlineMode) {
        if (!AuthClient::available()) {
            Log::error("online-mode requires libcurl support");
            running_.store(false);
            return false;
        }
        try {
            rsaKey_ = std::make_unique<RsaKey>();
        } catch (const std::exception& error) {
            Log::error("failed to initialize rsa key: {}", error.what());
            running_.store(false);
            return false;
        }
        Log::info("online-mode enabled");
    }

    if (!net_.start()) {
        Log::error("failed to start network layer");
        running_.store(false);
        return false;
    }

    generateVillage();
    spawnInitialMobs();

    if (config_.scriptsEnabled) {
        if (!scriptEngine_.loadDirectory(config_.scriptDir)) {
            Log::warn("script loading completed with errors");
        }
    }

    tickRunning_.store(true, std::memory_order_release);
    nextAutosave_ = std::chrono::steady_clock::now()
        + std::chrono::seconds(config_.autosaveSeconds > 0 ? config_.autosaveSeconds : 3600);
    tickThread_ = std::thread([this] { tickLoop(); });

    Log::info("minicraft {} ready (protocol {}, online-mode={}, view-distance={})",
              kServerVersion, kProtocolVersion, config_.onlineMode, config_.viewDistance);
    return true;
}

void Server::run() {
    bool consoleEnabled = true;
    while (running_.load()) {
        if (g_signalRequested != 0) {
            Log::info("shutdown signal received");
            requestStop();
            break;
        }
        if (!consoleEnabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            consoleEnabled = false;
            continue;
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            std::string line;
            if (std::getline(std::cin, line)) {
                if (!line.empty()) {
                    commands_.execute(nullptr, line);
                }
            } else {
                consoleEnabled = false;
            }
        }
    }
    stop();
}

void Server::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    requestStop();
    tickRunning_.store(false, std::memory_order_release);
    tickCondition_.notify_all();
    if (tickThread_.joinable()) {
        tickThread_.join();
    }
    authPool_.stop();
    net_.stop();
    world_.save();
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    players_.clear();
    Log::info("server stopped");
}

void Server::requestStop() {
    running_.store(false);
}

const ServerConfig& Server::config() const {
    return config_;
}

World& Server::world() {
    return world_;
}

PlayerManager& Server::players() {
    return players_;
}

ScriptEngine& Server::scripts() {
    return scriptEngine_;
}

bool Server::reloadScripts() {
    return scriptEngine_.reload(config_.scriptDir);
}

bool Server::isOp(std::string_view name) const {
    const std::string key = lowerName(name);
    std::shared_lock lock(opsMutex_);
    return ops_.contains(key);
}

void Server::setOp(std::string_view name, bool value) {
    const std::string key = lowerName(name);
    if (key.empty()) {
        return;
    }
    std::unique_lock lock(opsMutex_);
    if (value) {
        ops_.insert(key);
    } else {
        ops_.erase(key);
    }
    saveOps();
}

void Server::saveOps() {
    if (config_.opsFile.empty()) {
        return;
    }
    std::vector<std::string> names(ops_.begin(), ops_.end());
    std::sort(names.begin(), names.end());
    std::ofstream output(config_.opsFile, std::ios::trunc);
    if (!output) {
        Log::warn("failed to write ops file {}", config_.opsFile);
        return;
    }
    for (const auto& name : names) {
        output << name << '\n';
    }
}

void Server::loadOps() {
    std::unique_lock lock(opsMutex_);
    ops_.clear();
    std::string_view remaining = config_.ops;
    while (!remaining.empty()) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        std::size_t begin = 0;
        std::size_t end = token.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(token[begin])) != 0) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1])) != 0) {
            --end;
        }
        if (end > begin) {
            ops_.insert(lowerName(token.substr(begin, end - begin)));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    if (!config_.opsFile.empty()) {
        std::ifstream input(config_.opsFile);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::size_t begin = 0;
            std::size_t end = line.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
                ++begin;
            }
            while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
                --end;
            }
            if (end > begin) {
                ops_.insert(lowerName(std::string_view(line).substr(begin, end - begin)));
            }
        }
    }
    if (!ops_.empty()) {
        Log::info("loaded {} operators", ops_.size());
    }
}

std::int64_t Server::uptimeSeconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt_).count();
}

void Server::onOpen(Connection& connection) {
    connection.setViewDistance(config_.viewDistance);
    connection.setClientViewDistance(config_.viewDistance);
    connection.setTimeoutMs(config_.playerTimeoutMs);
    connection.setMaxPacketsPerSecond(config_.maxPacketsPerSecond);
    connection.setMaxOutputBuffer(config_.maxOutputBuffer);
    Log::debug("connection from {}", connection.remoteAddress());
}

void Server::onPacket(Connection& connection, std::int32_t packetId, ByteBuffer& payload) {
    switch (connection.state()) {
    case ConnectionState::Handshake:
        handleHandshake(connection, packetId, payload);
        break;
    case ConnectionState::Status:
        handleStatus(connection, packetId, payload);
        break;
    case ConnectionState::Login:
        handleLogin(connection, packetId, payload);
        break;
    case ConnectionState::Play:
        handlePlay(connection, packetId, payload);
        break;
    default:
        connection.requestClose();
        break;
    }
}

void Server::onTick(Connection& connection) {
    if (connection.state() != ConnectionState::Play) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - connection.lastTimeUpdate() >= std::chrono::seconds(1)) {
        sendTime(connection);
    }
    updateSurvival(connection);
}

void Server::onClose(Connection& connection) {
    if (!connection.authenticated()) {
        return;
    }
    dropCursor(connection, true);
    savePlayerData(connection);
    if (!players_.remove(connection.entityId())) {
        return;
    }
    const std::string name = connection.username();
    Log::info("{} left", name);
    broadcastEntityDestroy(connection.entityId());
    for (const auto& other : players_.snapshot()) {
        sendPlayerInfoRemove(*other, connection.uuid());
    }
    sendLeaveMessage(name);
    scriptEngine_.fire("on_leave", {}, {name});
}

void Server::onLegacyPing(Connection& connection) {
    if (!config_.legacyPing) {
        connection.requestClose();
        return;
    }
    std::string response = "\xC2\xA7" "1";
    response.push_back('\0');
    response += std::to_string(kProtocolVersion);
    response.push_back('\0');
    response += std::string(kServerVersion);
    response.push_back('\0');
    response += chat::section(config_.motd);
    response.push_back('\0');
    response += std::to_string(players_.count());
    response.push_back('\0');
    response += std::to_string(config_.maxPlayers);
    connection.sendLegacyPong(response);
    Log::debug("legacy ping from {}", connection.remoteAddress());
}

std::string Server::playerDataPath(const Uuid& uuid) const {
    const std::filesystem::path directory(config_.playerDataDir);
    return (directory / (uuid.toString() + ".json")).string();
}

bool Server::loadPlayerData(Connection& connection) {
    if (!config_.playerData) {
        return false;
    }
    std::lock_guard lock(playerDataMutex_);
    const std::string path = playerDataPath(connection.uuid());
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    try {
        const Json root = Json::parse(content);
        if (!root.isObject() || !root.contains("x") || !root.contains("y") || !root.contains("z")) {
            return false;
        }
        const double x = root["x"].asDouble();
        const double y = root["y"].asDouble();
        const double z = root["z"].asDouble();
        if (!validMovement(x, y, z)) {
            return false;
        }
        const float yaw = static_cast<float>(root["yaw"].asDouble(0.0));
        const float pitch = static_cast<float>(root["pitch"].asDouble(0.0));
        const int mode = std::clamp(static_cast<int>(root["gamemode"].asInt(config_.gameMode)), 0, 3);
        connection.setPosition(x, y, z);
        connection.setRotation(yaw, pitch);
        connection.setGameMode(mode);
        const float health = static_cast<float>(root["health"].asDouble(20.0));
        const int food = static_cast<int>(root["food"].asInt(20));
        const float saturation = static_cast<float>(root["saturation"].asDouble(5.0));
        connection.setHealth(health > 0.0F ? health : 20.0F);
        connection.setFood(food);
        connection.setSaturation(saturation);
        connection.setPlayerDead(false);

        std::array<ItemStack, 45> inventory{};
        if (root.contains("inventory") && root["inventory"].isArray()) {
            for (const auto& entry : root["inventory"].asArray()) {
                if (!entry.isObject()) {
                    continue;
                }
                const int slot = static_cast<int>(entry["slot"].asInt(-1));
                if (slot < 0 || slot >= 45) {
                    continue;
                }
                const int id = static_cast<int>(entry["id"].asInt(-1));
                const int count = static_cast<int>(entry["count"].asInt(0));
                const int damage = static_cast<int>(entry["damage"].asInt(0));
                if (id >= 0 && count > 0) {
                    inventory[static_cast<std::size_t>(slot)] = ItemStack{
                        static_cast<i16>(id),
                        static_cast<i8>(std::clamp(count, 1, 64)),
                        static_cast<i16>(damage)};
                }
            }
        }
        if (root.contains("cursor")) {
            Log::warn("discarding legacy cursor item for {}", connection.username());
        }
        connection.setInventoryFull(inventory, ItemStack{});
        connection.setHeldSlot(static_cast<int>(root["held-slot"].asInt(0)));

        Log::debug("loaded player data for {}", connection.username());
        return true;
    } catch (const std::exception& error) {
        Log::warn("failed to load player data for {}: {}", connection.username(), error.what());
        return false;
    }
}

void Server::savePlayerData(Connection& connection) {
    if (!config_.playerData || !connection.authenticated()) {
        return;
    }
    Json root = Json::object();
    root["name"] = connection.username();
    root["x"] = connection.x();
    root["y"] = connection.y();
    root["z"] = connection.z();
    root["yaw"] = static_cast<double>(connection.yaw());
    root["pitch"] = static_cast<double>(connection.pitch());
    root["gamemode"] = static_cast<std::int64_t>(connection.gameMode());
    root["held-slot"] = static_cast<std::int64_t>(connection.heldSlot());
    root["health"] = static_cast<double>(connection.health());
    root["food"] = static_cast<std::int64_t>(connection.food());
    root["saturation"] = static_cast<double>(connection.saturation());

    Json inventory = Json::array();
    const auto slots = connection.inventorySnapshot();
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        if (slots[slot].empty()) {
            continue;
        }
        Json entry = Json::object();
        entry["slot"] = static_cast<std::int64_t>(slot);
        entry["id"] = static_cast<std::int64_t>(slots[slot].id);
        entry["count"] = static_cast<std::int64_t>(slots[slot].count);
        entry["damage"] = static_cast<std::int64_t>(slots[slot].damage);
        inventory.push(std::move(entry));
    }
    root["inventory"] = std::move(inventory);

    std::lock_guard lock(playerDataMutex_);
    const std::filesystem::path directory(config_.playerDataDir);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        Log::warn("failed to create player data directory: {}", error.message());
        return;
    }
    const std::filesystem::path path = playerDataPath(connection.uuid());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        Log::warn("failed to write player data for {}", connection.username());
        return;
    }
    output << root.dump();
    output.close();
    if (!output) {
        Log::warn("failed to flush player data for {}", connection.username());
        return;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        Log::warn("failed to persist player data for {}", connection.username());
    }
}

void Server::saveAllPlayers() {
    for (const auto& player : players_.snapshot()) {
        savePlayerData(*player);
    }
}

void Server::clearPlayerInventory(const std::shared_ptr<Connection>& connection) {
    if (!connection) {
        return;
    }
    connection->clearInventory();
    broadcastEquipment(connection);
}

void Server::setBlockState(std::int32_t x, std::int32_t y, std::int32_t z, u16 blockState) {
    if (y < 0 || y > 255) {
        return;
    }
    const u16 previousState = world_.getBlock(x, y, z);
    world_.setBlock(x, y, z, blockState);
    broadcastBlockChange(x, y, z, blockState);
    scheduleFluidNeighbors(x, y, z);
    if (blockLightEmission(static_cast<u16>(previousState >> 4)) > 0
        || blockLightEmission(static_cast<u16>(blockState >> 4)) > 0) {
        refreshChunkAt(x, z);
    }
    if (blockState == kAir) {
        tryStartFalling(x, y + 1, z);
    } else {
        tryGrowGrassAt(x, y, z);
        tryStartFalling(x, y, z);
    }
}

void Server::cleanupItems() {
    std::vector<std::int32_t> ids;
    {
        std::lock_guard lock(itemsMutex_);
        ids.reserve(items_.size());
        for (const auto& item : items_) {
            ids.push_back(item.entityId);
        }
        items_.clear();
    }
    if (ids.empty()) {
        return;
    }
    for (const auto& player : players_.snapshot()) {
        player->send(0x13, [&](ByteBuffer& buffer) {
            buffer.writeVarInt(static_cast<i32>(ids.size()));
            for (const auto id : ids) {
                buffer.writeVarInt(id);
            }
        });
    }
}

void Server::tickLoop() {
    pthread_setname_np(pthread_self(), "mc-tick");
    auto nextTick = std::chrono::steady_clock::now();
    auto nextGrassTick = nextTick + std::chrono::seconds(5);
    auto nextScriptTick = nextTick
        + std::chrono::milliseconds(config_.scriptTickIntervalMs > 0 ? config_.scriptTickIntervalMs : 1000);
    std::int64_t scriptTicks = 0;
    while (tickRunning_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(tickMutex_);
            tickCondition_.wait_until(lock, nextTick, [this] { return !tickRunning_.load(); });
            if (!tickRunning_.load()) {
                break;
            }
        }
        updateItems();
        updateFallingBlocks();
        updateMobs();
        updateFluids();
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextGrassTick) {
                nextGrassTick = now + std::chrono::seconds(5);
                for (const auto& change : world_.randomTick()) {
                    broadcastBlockChange(change.x, change.y, change.z, change.state);
                }
            }
        }
        if (config_.autosaveSeconds > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextAutosave_) {
                saveAllPlayers();
                world_.save();
#if defined(__GLIBC__)
                malloc_trim(0);
#endif
                nextAutosave_ = now + std::chrono::seconds(config_.autosaveSeconds);
            }
        }
        if (config_.scriptsEnabled) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextScriptTick) {
                nextScriptTick = now + std::chrono::milliseconds(
                    config_.scriptTickIntervalMs > 0 ? config_.scriptTickIntervalMs : 1000);
                scriptEngine_.fire("on_tick", {++scriptTicks});
            }
        }
        nextTick += std::chrono::milliseconds(50);
        const auto now = std::chrono::steady_clock::now();
        if (nextTick < now) {
            nextTick = now + std::chrono::milliseconds(50);
        }
    }
}

void Server::updateItems() {
    std::vector<ItemEntity> updates;
    std::vector<std::int32_t> destroyed;
    std::vector<std::pair<std::int32_t, std::int32_t>> collected;
    std::vector<ItemEntity> metadataUpdates;
    std::vector<std::shared_ptr<Connection>> equipmentUpdates;
    const auto players = players_.snapshot();
    {
        std::lock_guard lock(itemsMutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = items_.begin(); it != items_.end();) {
            if ((config_.itemDespawnSeconds > 0
                    && now - it->spawned > std::chrono::seconds(config_.itemDespawnSeconds))
                || it->y < -16.0) {
                destroyed.push_back(it->entityId);
                it = items_.erase(it);
                continue;
            }

            if (!it->resting) {
                it->velocityY -= 0.04;
                it->velocityX *= 0.98;
                it->velocityZ *= 0.98;

                const auto blockY = static_cast<i32>(std::floor(it->y));
                const auto blockZ = static_cast<i32>(std::floor(it->z));
                const double nextX = it->x + it->velocityX;
                if (world_.getBlock(static_cast<i32>(std::floor(nextX)), blockY, blockZ) == kAir) {
                    it->x = nextX;
                } else {
                    it->velocityX = 0.0;
                }
                const auto blockX = static_cast<i32>(std::floor(it->x));
                const double nextZ = it->z + it->velocityZ;
                if (world_.getBlock(blockX, blockY, static_cast<i32>(std::floor(nextZ))) == kAir) {
                    it->z = nextZ;
                } else {
                    it->velocityZ = 0.0;
                }

                const double nextY = it->y + it->velocityY;
                const auto feetY = static_cast<i32>(std::floor(nextY));
                if (world_.getBlock(blockX, feetY, blockZ) != kAir) {
                    if (it->velocityY <= 0.0) {
                        it->y = static_cast<double>(feetY) + 1.0;
                        it->resting = true;
                    }
                    it->velocityY = 0.0;
                } else {
                    it->y = nextY;
                }
            }

            if (now >= it->pickupAt && it->count > 0) {
                std::shared_ptr<Connection> collector;
                const double pickupRadius = config_.itemPickupRadius;
                const double pickupRadiusSquared = pickupRadius * pickupRadius;
                double bestDistance = pickupRadiusSquared + 0.0001;
                for (const auto& player : players) {
                    if (!player || player->dead() || player->state() != ConnectionState::Play
                        || player->gameMode() == 3) {
                        continue;
                    }
                    const double dx = player->x() - it->x;
                    const double dy = player->y() - it->y;
                    const double dz = player->z() - it->z;
                    if (std::abs(dy) > 2.0) {
                        continue;
                    }
                    const double distance = dx * dx + dy * dy + dz * dz;
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        collector = player;
                    }
                }
                if (collector) {
                    const int original = it->count;
                    const int remaining = collector->addItem(
                        ItemStack{static_cast<i16>(it->itemId), it->count, static_cast<i16>(it->damage)});
                    if (remaining != original) {
                        equipmentUpdates.push_back(collector);
                    }
                    if (remaining == 0) {
                        collected.emplace_back(it->entityId, collector->entityId());
                        it = items_.erase(it);
                        continue;
                    }
                    if (remaining != original) {
                        it->count = static_cast<i8>(remaining);
                        it->pickupAt = now + std::chrono::milliseconds(200);
                        metadataUpdates.push_back(*it);
                    }
                }
            }

            if (!it->resting) {
                updates.push_back(*it);
                it->lastX = it->x;
                it->lastY = it->y;
                it->lastZ = it->z;
            }
            ++it;
        }
    }
    for (const auto& item : updates) {
        sendItemMovement(item);
    }
    for (const auto& item : metadataUpdates) {
        sendItemMetadata(item);
    }
    for (const auto id : destroyed) {
        sendItemDestroy(id);
    }
    for (const auto& [collectedId, collectorId] : collected) {
        for (const auto& player : players) {
            if (config_.itemPickupAnimation) {
                player->send(0x0D, [&](ByteBuffer& buffer) {
                    buffer.writeVarInt(collectedId);
                    buffer.writeVarInt(collectorId);
                });
            } else {
                player->send(0x13, [&](ByteBuffer& buffer) {
                    buffer.writeVarInt(1);
                    buffer.writeVarInt(collectedId);
                });
            }
        }
    }
    std::unordered_set<std::int32_t> updated;
    for (const auto& player : equipmentUpdates) {
        if (player && updated.insert(player->entityId()).second) {
            broadcastEquipment(player);
        }
    }
}

void Server::tryGrowGrassAt(std::int32_t x, std::int32_t y, std::int32_t z) {
    const u16 state = world_.getBlock(x, y, z);
    const u16 id = static_cast<u16>(state >> 4);
    static constexpr i32 offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    if (id == 2) {
        if (world_.getBlock(x, y + 1, z) != kAir) {
            return;
        }
        for (const auto& offset : offsets) {
            const i32 nx = x + offset[0];
            const i32 nz = z + offset[1];
            if (world_.getBlock(nx, y, nz) == (3 << 4) && world_.getBlock(nx, y + 1, nz) == kAir) {
                world_.setBlock(nx, y, nz, static_cast<u16>(2 << 4));
                broadcastBlockChange(nx, y, nz, static_cast<u16>(2 << 4));
                return;
            }
        }
    } else if (id == 3) {
        if (world_.getBlock(x, y + 1, z) != kAir) {
            return;
        }
        for (const auto& offset : offsets) {
            const i32 nx = x + offset[0];
            const i32 nz = z + offset[1];
            if (world_.getBlock(nx, y, nz) == (2 << 4) && world_.getBlock(nx, y + 1, nz) == kAir) {
                world_.setBlock(x, y, z, static_cast<u16>(2 << 4));
                broadcastBlockChange(x, y, z, static_cast<u16>(2 << 4));
                return;
            }
        }
    }
}

bool Server::consumeFood(Connection& connection, const ItemStack& stack) {
    if (stack.empty() || connection.food() >= 20) {
        return false;
    }
    int foodValue = 0;
    float saturationValue = 0.0F;
    switch (stack.id) {
    case 260: foodValue = 4; saturationValue = 2.4F; break;
    case 282: foodValue = 6; saturationValue = 7.2F; break;
    case 297: foodValue = 5; saturationValue = 6.0F; break;
    case 319: foodValue = 3; saturationValue = 1.8F; break;
    case 320: foodValue = 8; saturationValue = 12.8F; break;
    case 322: foodValue = 4; saturationValue = 9.6F; break;
    case 349: foodValue = 2; saturationValue = 0.4F; break;
    case 350: foodValue = 5; saturationValue = 6.0F; break;
    case 357: foodValue = 2; saturationValue = 0.4F; break;
    case 360: foodValue = 2; saturationValue = 1.2F; break;
    case 363: foodValue = 3; saturationValue = 1.8F; break;
    case 364: foodValue = 8; saturationValue = 12.8F; break;
    case 365: foodValue = 2; saturationValue = 1.2F; break;
    case 366: foodValue = 6; saturationValue = 7.2F; break;
    case 375: foodValue = 2; saturationValue = 3.2F; break;
    case 391: foodValue = 3; saturationValue = 3.6F; break;
    case 392: foodValue = 1; saturationValue = 0.6F; break;
    case 393: foodValue = 5; saturationValue = 6.0F; break;
    case 394: foodValue = 2; saturationValue = 1.2F; break;
    case 400: foodValue = 8; saturationValue = 4.8F; break;
    case 413: foodValue = 10; saturationValue = 12.0F; break;
    case 423: foodValue = 2; saturationValue = 1.2F; break;
    case 424: foodValue = 6; saturationValue = 9.6F; break;
    default:
        return false;
    }
    connection.setFood(connection.food() + foodValue);
    connection.setSaturation(connection.saturation() + saturationValue);
    sendHealth(connection);
    return true;
}

void Server::useBucket(Connection& connection, const ItemStack& stack, std::int32_t x, std::int32_t y, std::int32_t z, i8 face) {
    static constexpr i32 offsets[6][3] = {
        {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
    };
    if (face < 0 || face > 5) {
        return;
    }
    const i32 targetX = x + offsets[static_cast<std::size_t>(face)][0];
    const i32 targetY = y + offsets[static_cast<std::size_t>(face)][1];
    const i32 targetZ = z + offsets[static_cast<std::size_t>(face)][2];
    if (targetY < 0 || targetY > 255 || world_.getBlock(targetX, targetY, targetZ) != kAir) {
        return;
    }
    const bool water = stack.id == 326;
    const u16 source = water ? static_cast<u16>(9 << 4) : static_cast<u16>(11 << 4);
    world_.setBlock(targetX, targetY, targetZ, source);
    broadcastBlockChange(targetX, targetY, targetZ, source);
    scheduleFluidNeighbors(targetX, targetY, targetZ);
    if (!water) {
        refreshChunkAt(targetX, targetZ);
    }
    if (connection.gameMode() != 1) {
        connection.takeHeldItem(1);
        connection.addItem(ItemStack{325, 1, 0});
        broadcastEquipment(connection.shared_from_this());
    }
}

void Server::tryStartFalling(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (!config_.sandPhysics || y <= 0 || y > 255) {
        return;
    }
    {
        std::lock_guard lock(fallingMutex_);
        if (static_cast<int>(fallingBlocks_.size()) >= config_.maxFallingBlocks) {
            return;
        }
    }

    u16 state = kAir;
    {
        std::lock_guard lock(fallingStartMutex_);
        state = world_.getBlock(x, y, z);
        const u16 blockId = static_cast<u16>(state >> 4);
        if (blockId != 12 && blockId != 13) {
            return;
        }
        if (world_.getBlock(x, y - 1, z) != kAir) {
            return;
        }
        world_.setBlock(x, y, z, kAir);
    }
    broadcastBlockChange(x, y, z, kAir);
    scheduleFluidNeighbors(x, y, z);

    FallingBlock block;
    block.entityId = nextEntityId_.fetch_add(1);
    block.blockState = state;
    block.blockX = x;
    block.blockZ = z;
    block.x = static_cast<double>(x) + 0.5;
    block.y = static_cast<double>(y) + 0.5;
    block.z = static_cast<double>(z) + 0.5;
    block.lastX = block.x;
    block.lastY = block.y;
    block.lastZ = block.z;

    {
        std::lock_guard lock(fallingMutex_);
        fallingBlocks_.push_back(block);
    }
    for (const auto& player : players_.snapshot()) {
        sendFallingSpawn(*player, block);
    }

    tryStartFalling(x, y + 1, z);
}

void Server::updateFallingBlocks() {
    struct LandedBlock {
        std::int32_t entityId{0};
        u16 blockState{0};
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t z{0};
    };

    std::vector<FallingBlock> updates;
    std::vector<std::int32_t> destroyed;
    std::vector<LandedBlock> landed;
    {
        std::lock_guard lock(fallingMutex_);
        for (auto it = fallingBlocks_.begin(); it != fallingBlocks_.end();) {
            it->velocityY -= 0.04;
            it->y += it->velocityY;

            if (it->y < -16.0) {
                destroyed.push_back(it->entityId);
                it = fallingBlocks_.erase(it);
                continue;
            }

            const auto centerCell = static_cast<i32>(std::floor(it->y));
            const u16 below = world_.getBlock(it->blockX, centerCell - 1, it->blockZ);
            if (below != kAir && it->velocityY <= 0.0) {
                const i32 landY = centerCell;
                if (landY >= 0 && landY < 256 && world_.getBlock(it->blockX, landY, it->blockZ) == kAir) {
                    world_.setBlock(it->blockX, landY, it->blockZ, it->blockState);
                    landed.push_back(LandedBlock{it->entityId, it->blockState, it->blockX, landY, it->blockZ});
                }
                destroyed.push_back(it->entityId);
                it = fallingBlocks_.erase(it);
                continue;
            }

            updates.push_back(*it);
            it->lastX = it->x;
            it->lastY = it->y;
            it->lastZ = it->z;
            ++it;
        }
    }

    for (const auto& block : updates) {
        sendFallingMovement(block);
    }
    for (const auto id : destroyed) {
        sendItemDestroy(id);
    }
    for (const auto& block : landed) {
        broadcastBlockChange(block.x, block.y, block.z, block.blockState);
        tryStartFalling(block.x, block.y + 1, block.z);
    }
}

void Server::sendFallingSpawn(Connection& connection, const FallingBlock& block) {
    connection.send(0x0E, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(block.entityId);
        buffer.writeI8(70);
        buffer.writeI32(fixedPoint(block.x));
        buffer.writeI32(fixedPoint(block.y));
        buffer.writeI32(fixedPoint(block.z));
        buffer.writeI8(0);
        buffer.writeI8(0);
        buffer.writeI32(block.blockState);
        buffer.writeI16(0);
        buffer.writeI16(0);
        buffer.writeI16(0);
    });
}

void Server::sendFallingMovement(const FallingBlock& block) {
    const double deltaX = block.x - block.lastX;
    const double deltaY = block.y - block.lastY;
    const double deltaZ = block.z - block.lastZ;
    if (std::abs(deltaX) < 1.0 / 64.0 && std::abs(deltaY) < 1.0 / 64.0 && std::abs(deltaZ) < 1.0 / 64.0) {
        return;
    }
    const bool teleport = std::abs(deltaX) > 3.9 || std::abs(deltaY) > 3.9 || std::abs(deltaZ) > 3.9;
    const i8 dx = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaX * 32.0)), -128, 127));
    const i8 dy = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaY * 32.0)), -128, 127));
    const i8 dz = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaZ * 32.0)), -128, 127));
    for (const auto& player : players_.snapshot()) {
        if (teleport) {
            player->send(0x18, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(block.entityId);
                buffer.writeI32(fixedPoint(block.x));
                buffer.writeI32(fixedPoint(block.y));
                buffer.writeI32(fixedPoint(block.z));
                buffer.writeI8(0);
                buffer.writeI8(0);
                buffer.writeBool(false);
            });
        } else {
            player->send(0x15, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(block.entityId);
                buffer.writeI8(dx);
                buffer.writeI8(dy);
                buffer.writeI8(dz);
                buffer.writeBool(false);
            });
        }
    }
}

void Server::sendActiveFallingBlocks(Connection& connection) {
    std::vector<FallingBlock> snapshot;
    {
        std::lock_guard lock(fallingMutex_);
        snapshot = fallingBlocks_;
    }
    for (const auto& block : snapshot) {
        sendFallingSpawn(connection, block);
    }
}

void Server::scheduleFluidUpdate(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (!config_.fluidsEnabled || y < 0 || y > 255) {
        return;
    }
    std::lock_guard lock(fluidMutex_);
    fluidUpdates_.insert(encodeFluidKey(x, y, z));
}

void Server::scheduleFluidNeighbors(std::int32_t x, std::int32_t y, std::int32_t z) {
    scheduleFluidUpdate(x, y, z);
    scheduleFluidUpdate(x + 1, y, z);
    scheduleFluidUpdate(x - 1, y, z);
    scheduleFluidUpdate(x, y + 1, z);
    scheduleFluidUpdate(x, y - 1, z);
    scheduleFluidUpdate(x, y, z + 1);
    scheduleFluidUpdate(x, y, z - 1);
}

void Server::updateFluids() {
    if (!config_.fluidsEnabled) {
        return;
    }
    ++fluidTickCounter_;
    if (fluidTickCounter_ % static_cast<std::uint64_t>(config_.fluidTickInterval) != 0) {
        return;
    }

    std::vector<u64> batch;
    batch.reserve(static_cast<std::size_t>(config_.fluidUpdatesPerTick));
    {
        std::lock_guard lock(fluidMutex_);
        while (!fluidUpdates_.empty() && static_cast<int>(batch.size()) < config_.fluidUpdatesPerTick) {
            const u64 key = *fluidUpdates_.begin();
            fluidUpdates_.erase(fluidUpdates_.begin());
            batch.push_back(key);
        }
    }

    const auto placeFluid = [&](std::int32_t x, std::int32_t y, std::int32_t z, u16 state, bool water) {
        if (y < 0 || y > 255) {
            return;
        }
        const u16 target = world_.getBlock(x, y, z);
        if (target != kAir) {
            const u16 targetId = static_cast<u16>(target >> 4);
            const bool targetLava = targetId == 10 || targetId == 11;
            const bool targetWater = targetId == 8 || targetId == 9;
            if (water && targetLava) {
                const u16 result = (targetId == 11 && (target & 0x0F) == 0) ? static_cast<u16>(49 << 4) : static_cast<u16>(4 << 4);
                world_.setBlock(x, y, z, result);
                broadcastBlockChange(x, y, z, result);
                scheduleFluidNeighbors(x, y, z);
                return;
            }
            if (!water && targetWater) {
                const u16 result = static_cast<u16>(4 << 4);
                world_.setBlock(x, y, z, result);
                broadcastBlockChange(x, y, z, result);
                scheduleFluidNeighbors(x, y, z);
                return;
            }
            return;
        }
        world_.setBlock(x, y, z, state);
        broadcastBlockChange(x, y, z, state);
        scheduleFluidUpdate(x, y, z);
        if (blockLightEmission(static_cast<u16>(state >> 4)) > 0) {
            refreshChunkAt(x, z);
        }
    };

    for (const u64 key : batch) {
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
        decodeFluidKey(key, x, y, z);
        const u16 state = world_.getBlock(x, y, z);
        const u16 blockId = static_cast<u16>(state >> 4);
        if (blockId != 8 && blockId != 9 && blockId != 10 && blockId != 11) {
            continue;
        }
        const bool water = blockId == 8 || blockId == 9;
        const bool source = blockId == 9 || blockId == 11;
        const u16 meta = static_cast<u16>(state & 0x0F);
        const int level = source ? 0 : (meta == 8 ? 1 : static_cast<int>(meta));
        const int maxLevel = water ? 7 : 3;
        const u16 flowingId = water ? 8 : 10;
        const u16 sourceId = water ? 9 : 11;

        if (!source) {
            bool supported = false;
            const u16 aboveId = static_cast<u16>(world_.getBlock(x, y + 1, z) >> 4);
            if (aboveId == sourceId || aboveId == flowingId) {
                supported = true;
            }
            if (!supported) {
                const auto neighborSupports = [&](int nx, int nz) {
                    const u16 neighbor = world_.getBlock(nx, y, nz);
                    const u16 neighborId = static_cast<u16>(neighbor >> 4);
                    if (neighborId == sourceId) {
                        return true;
                    }
                    if (neighborId == flowingId) {
                        const int neighborLevel = (neighbor & 0x0F) == 8 ? 0 : static_cast<int>(neighbor & 0x0F);
                        return neighborLevel < level;
                    }
                    return false;
                };
                supported = neighborSupports(x + 1, z) || neighborSupports(x - 1, z)
                    || neighborSupports(x, z + 1) || neighborSupports(x, z - 1);
            }
            if (!supported) {
                Log::trace("fluid recede {},{},{}", x, y, z);
                world_.setBlock(x, y, z, kAir);
                broadcastBlockChange(x, y, z, kAir);
                scheduleFluidNeighbors(x, y, z);
                continue;
            }
        }

        if (y > 0) {
            const u16 below = world_.getBlock(x, y - 1, z);
            if (below == kAir || (below >> 4) == 8 || (below >> 4) == 10) {
                placeFluid(x, y - 1, z, static_cast<u16>((flowingId << 4) | 8), water);
            }
        }

        if (level < maxLevel) {
            const u16 nextMeta = static_cast<u16>(source ? 1 : level + 1);
            placeFluid(x + 1, y, z, static_cast<u16>((flowingId << 4) | nextMeta), water);
            placeFluid(x - 1, y, z, static_cast<u16>((flowingId << 4) | nextMeta), water);
            placeFluid(x, y, z + 1, static_cast<u16>((flowingId << 4) | nextMeta), water);
            placeFluid(x, y, z - 1, static_cast<u16>((flowingId << 4) | nextMeta), water);
        }
    }
}

void Server::kickExistingByName(std::string_view name) {
    const auto existing = players_.byName(name);
    if (!existing) {
        return;
    }
    Log::warn("replacing existing session of {} from {}", existing->username(), existing->remoteAddress());
    const std::int32_t entityId = existing->entityId();
    const Uuid uuid = existing->uuid();
    players_.remove(entityId);
    broadcastEntityDestroy(entityId);
    for (const auto& player : players_.snapshot()) {
        sendPlayerInfoRemove(*player, uuid);
    }
    existing->disconnect(chat::text("You logged in from another location"));
}

void Server::summonMob(u8 type, double x, double y, double z) {
    spawnMob(type, x, y, z, 0.0F);
}

void Server::spawnMob(u8 type, double x, double y, double z, float yaw) {
    MobEntity mob;
    mob.entityId = nextEntityId_.fetch_add(1);
    mob.type = type;
    mob.x = x;
    mob.y = y;
    mob.z = z;
    mob.yaw = yaw;
    mob.lastX = x;
    mob.lastY = y;
    mob.lastZ = z;
    mob.lastYaw = yaw;
    mob.health = type == 120 ? 20.0F : (type == 93 ? 4.0F : 10.0F);
    mob.wanderTicks = static_cast<int>(mobRng_() % 60) + 20;

    {
        std::lock_guard lock(mobsMutex_);
        if (static_cast<int>(mobs_.size()) >= config_.maxMobs) {
            return;
        }
        mobs_.push_back(mob);
    }
    for (const auto& player : players_.snapshot()) {
        sendMobSpawn(*player, mob);
    }
}

void Server::sendMobSpawn(Connection& connection, const MobEntity& mob) {
    connection.send(0x0F, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(mob.entityId);
        buffer.writeU8(mob.type);
        buffer.writeI32(fixedPoint(mob.x));
        buffer.writeI32(fixedPoint(mob.y));
        buffer.writeI32(fixedPoint(mob.z));
        buffer.writeI8(angleByte(mob.yaw));
        buffer.writeI8(0);
        buffer.writeI8(0);
        buffer.writeI16(0);
        buffer.writeI16(0);
        buffer.writeI16(0);
        buffer.writeU8(0x7F);
    });
}

void Server::sendMobMovement(const MobEntity& mob) {
    const double deltaX = mob.x - mob.lastX;
    const double deltaY = mob.y - mob.lastY;
    const double deltaZ = mob.z - mob.lastZ;
    const i8 yawByte = angleByte(mob.yaw);
    const bool rotated = yawByte != angleByte(mob.lastYaw);
    if (std::abs(deltaX) < 1.0 / 64.0 && std::abs(deltaY) < 1.0 / 64.0
        && std::abs(deltaZ) < 1.0 / 64.0 && !rotated) {
        return;
    }
    const bool teleport = std::abs(deltaX) > 3.9 || std::abs(deltaY) > 3.9 || std::abs(deltaZ) > 3.9;
    const i8 dx = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaX * 32.0)), -128, 127));
    const i8 dy = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaY * 32.0)), -128, 127));
    const i8 dz = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaZ * 32.0)), -128, 127));
    for (const auto& player : players_.snapshot()) {
        if (teleport) {
            player->send(0x18, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(mob.entityId);
                buffer.writeI32(fixedPoint(mob.x));
                buffer.writeI32(fixedPoint(mob.y));
                buffer.writeI32(fixedPoint(mob.z));
                buffer.writeI8(yawByte);
                buffer.writeI8(0);
                buffer.writeBool(mob.onGround);
            });
        } else {
            player->send(0x17, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(mob.entityId);
                buffer.writeI8(dx);
                buffer.writeI8(dy);
                buffer.writeI8(dz);
                buffer.writeI8(yawByte);
                buffer.writeI8(0);
                buffer.writeBool(mob.onGround);
            });
        }
        player->send(0x19, [&](ByteBuffer& buffer) {
            buffer.writeVarInt(mob.entityId);
            buffer.writeI8(yawByte);
        });
    }
}

void Server::updateMobs() {
    std::vector<MobEntity> updates;
    std::vector<std::int32_t> destroyed;
    {
        std::lock_guard lock(mobsMutex_);
        for (auto it = mobs_.begin(); it != mobs_.end();) {
            if (it->y < -16.0) {
                destroyed.push_back(it->entityId);
                it = mobs_.erase(it);
                continue;
            }

            const auto cellY = static_cast<i32>(std::floor(it->y));
            const auto cellX = static_cast<i32>(std::floor(it->x));
            const auto cellZ = static_cast<i32>(std::floor(it->z));
            if (world_.getBlock(cellX, cellY - 1, cellZ) != kAir) {
                it->y = static_cast<double>(cellY);
                it->velocityY = 0.0;
                it->onGround = true;
            } else {
                it->velocityY -= 0.04;
                it->y += it->velocityY;
                const auto nextY = static_cast<i32>(std::floor(it->y));
                if (world_.getBlock(cellX, nextY - 1, cellZ) != kAir) {
                    it->y = static_cast<double>(nextY);
                    it->velocityY = 0.0;
                    it->onGround = true;
                } else {
                    it->onGround = false;
                }
            }

            if (--it->wanderTicks <= 0) {
                it->wanderTicks = static_cast<int>(mobRng_() % 80) + 20;
                if (mobRng_() % 4 != 0) {
                    it->velocityX = static_cast<double>(static_cast<int>(mobRng_() % 101) - 50) / 1000.0;
                    it->velocityZ = static_cast<double>(static_cast<int>(mobRng_() % 101) - 50) / 1000.0;
                    if (it->velocityX != 0.0 || it->velocityZ != 0.0) {
                        it->yaw = static_cast<float>(std::atan2(-it->velocityX, it->velocityZ) * 57.2957795);
                    }
                } else {
                    it->velocityX = 0.0;
                    it->velocityZ = 0.0;
                }
            }

            const bool tall = it->type == 120 || it->type == 54 || it->type == 91 || it->type == 92;
            const auto blocked = [&](double nx, double ny, double nz) {
                const auto bx = static_cast<i32>(std::floor(nx));
                const auto by = static_cast<i32>(std::floor(ny));
                const auto bz = static_cast<i32>(std::floor(nz));
                if (world_.getBlock(bx, by, bz) != kAir) {
                    return true;
                }
                return tall && world_.getBlock(bx, by + 1, bz) != kAir;
            };
            const double nextX = it->x + it->velocityX;
            if (!blocked(nextX, it->y, it->z)) {
                it->x = nextX;
            } else {
                it->velocityX = 0.0;
            }
            const double nextZ = it->z + it->velocityZ;
            if (!blocked(it->x, it->y, nextZ)) {
                it->z = nextZ;
            } else {
                it->velocityZ = 0.0;
            }

            updates.push_back(*it);
            it->lastX = it->x;
            it->lastY = it->y;
            it->lastZ = it->z;
            it->lastYaw = it->yaw;
            ++it;
        }
    }
    for (const auto& mob : updates) {
        sendMobMovement(mob);
    }
    for (const auto id : destroyed) {
        sendItemDestroy(id);
    }
}

void Server::sendActiveMobs(Connection& connection) {
    std::vector<MobEntity> snapshot;
    {
        std::lock_guard lock(mobsMutex_);
        snapshot = mobs_;
    }
    for (const auto& mob : snapshot) {
        sendMobSpawn(connection, mob);
    }
}

void Server::attackMob(Connection& connection, std::int32_t entityId) {
    MobEntity deadMob;
    bool killed = false;
    {
        std::lock_guard lock(mobsMutex_);
        for (auto it = mobs_.begin(); it != mobs_.end(); ++it) {
            if (it->entityId != entityId) {
                continue;
            }
            const double dx = it->x - connection.x();
            const double dy = it->y - connection.y();
            const double dz = it->z - connection.z();
            if (dx * dx + dy * dy + dz * dz > 25.0) {
                return;
            }
            const double distance = std::sqrt(dx * dx + dz * dz);
            const double normalize = distance < 0.001 ? 1.0 : distance;
            it->velocityX = dx / normalize * 0.5;
            it->velocityZ = dz / normalize * 0.5;
            it->velocityY = 0.4;
            it->wanderTicks = 40;
            it->health -= 4.0F;
            if (it->health <= 0.0F) {
                deadMob = *it;
                killed = true;
                mobs_.erase(it);
            }
            break;
        }
    }
    if (!killed) {
        const auto velocity = [](double value) {
            return static_cast<i16>(std::clamp(static_cast<int>(std::lround(value * 8000.0)), -32768, 32767));
        };
        double velocityX = 0.0;
        double velocityY = 0.0;
        double velocityZ = 0.0;
        {
            std::lock_guard lock(mobsMutex_);
            for (const auto& mob : mobs_) {
                if (mob.entityId == entityId) {
                    velocityX = mob.velocityX;
                    velocityY = mob.velocityY;
                    velocityZ = mob.velocityZ;
                    break;
                }
            }
        }
        for (const auto& player : players_.snapshot()) {
            player->send(0x12, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(entityId);
                buffer.writeI16(velocity(velocityX));
                buffer.writeI16(velocity(velocityY));
                buffer.writeI16(velocity(velocityZ));
            });
            player->send(0x1A, [&](ByteBuffer& buffer) {
                buffer.writeI32(entityId);
                buffer.writeI8(2);
            });
        }
        return;
    }
    sendItemDestroy(entityId);
    i16 dropId = 0;
    switch (deadMob.type) {
    case 90: dropId = 319; break;
    case 91: dropId = 423; break;
    case 92: dropId = 363; break;
    case 93: dropId = 365; break;
    case 54: dropId = 367; break;
    default: break;
    }
    if (dropId != 0) {
        spawnItemEntity(ItemStack{dropId, 1, 0}, deadMob.x, deadMob.y + 0.5, deadMob.z, 0.02, 0.15, -0.02);
    }
}

void Server::generateVillage() {
    constexpr i32 villageX = 32;
    constexpr i32 villageZ = 0;
    const u16 marker = static_cast<u16>(19 << 4);
    if (world_.getBlock(villageX, 1, villageZ) == marker) {
        return;
    }

    const u16 planks = static_cast<u16>(5 << 4);
    const u16 cobble = static_cast<u16>(4 << 4);
    const u16 gravel = static_cast<u16>(13 << 4);
    const u16 torch = static_cast<u16>((50 << 4) | 5);

    for (i32 x = -10; x <= 10; ++x) {
        for (i32 z = -10; z <= 10; ++z) {
            const i32 distance = std::max(std::abs(x), std::abs(z));
            if (distance <= 3 || (x >= -3 && x <= 3) || (z >= -3 && z <= 3)) {
                world_.setBlock(villageX + x, 3, villageZ + z, gravel);
            }
        }
    }

    for (i32 x = -3; x <= 3; ++x) {
        for (i32 z = -3; z <= 3; ++z) {
            world_.setBlock(villageX + x, 3, villageZ + z, planks);
        }
    }
    for (i32 y = 4; y <= 5; ++y) {
        for (i32 x = -3; x <= 3; ++x) {
            for (i32 z = -3; z <= 3; ++z) {
                if (std::abs(x) == 3 || std::abs(z) == 3) {
                    world_.setBlock(villageX + x, y, villageZ + z, cobble);
                }
            }
        }
    }
    for (i32 x = -3; x <= 3; ++x) {
        for (i32 z = -3; z <= 3; ++z) {
            world_.setBlock(villageX + x, 6, villageZ + z, planks);
        }
    }
    world_.setBlock(villageX, 4, villageZ + 3, kAir);
    world_.setBlock(villageX, 5, villageZ + 3, kAir);
    world_.setBlock(villageX - 2, 4, villageZ - 2, torch);
    world_.setBlock(villageX + 2, 4, villageZ + 2, torch);

    const i32 house2X = villageX + 8;
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -2; z <= 2; ++z) {
            world_.setBlock(house2X + x, 3, villageZ + z, planks);
        }
    }
    for (i32 y = 4; y <= 5; ++y) {
        for (i32 x = -2; x <= 2; ++x) {
            for (i32 z = -2; z <= 2; ++z) {
                if (std::abs(x) == 2 || std::abs(z) == 2) {
                    world_.setBlock(house2X + x, y, villageZ + z, cobble);
                }
            }
        }
    }
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -2; z <= 2; ++z) {
            world_.setBlock(house2X + x, 6, villageZ + z, planks);
        }
    }
    world_.setBlock(house2X, 4, villageZ - 2, kAir);
    world_.setBlock(house2X, 5, villageZ - 2, kAir);
    world_.setBlock(house2X - 1, 4, villageZ - 1, torch);

    world_.setBlock(villageX, 1, villageZ, marker);
    Log::info("generated village at {},{}", villageX, villageZ);
}

void Server::spawnInitialMobs() {
    spawnMob(90, world_.spawnX() + 5.5, 4.0, world_.spawnZ() + 0.5, 90.0F);
    spawnMob(92, world_.spawnX() - 5.5, 4.0, world_.spawnZ() + 2.5, 180.0F);
    spawnMob(91, world_.spawnX() + 2.5, 4.0, world_.spawnZ() + 6.5, 270.0F);
    spawnMob(93, world_.spawnX() - 3.5, 4.0, world_.spawnZ() - 4.5, 0.0F);
    spawnMob(120, 32.5, 4.0, 2.5, 90.0F);
    spawnMob(120, 32.5, 4.0, -2.5, 270.0F);
    spawnMob(120, 40.5, 4.0, 0.5, 180.0F);
}

void Server::spawnItemDrop(u16 blockState, std::int32_t x, std::int32_t y, std::int32_t z) {
    if (!config_.itemDrops) {
        return;
    }
    const ItemStack drop = resolveBlockDrop(blockState, x, y, z);
    if (drop.empty()) {
        return;
    }
    const int spreadX = (x * 31 + z * 17) % 5 - 2;
    const int spreadZ = (x * 13 + z * 29) % 5 - 2;
    spawnItemEntity(drop,
                    static_cast<double>(x) + 0.5,
                    static_cast<double>(y) + 0.5,
                    static_cast<double>(z) + 0.5,
                    static_cast<double>(spreadX) * 0.01,
                    0.15,
                    static_cast<double>(spreadZ) * 0.01);
}

void Server::spawnItemEntity(const ItemStack& stack, double x, double y, double z,
                             double velocityX, double velocityY, double velocityZ) {
    if (stack.empty()) {
        return;
    }
    ItemEntity item;
    item.entityId = nextEntityId_.fetch_add(1);
    item.itemId = static_cast<u16>(stack.id);
    item.damage = static_cast<u16>(stack.damage);
    item.count = std::clamp<i8>(stack.count, 1, 64);
    item.x = x;
    item.y = y;
    item.z = z;
    item.lastX = x;
    item.lastY = y;
    item.lastZ = z;
    item.velocityX = velocityX;
    item.velocityY = velocityY;
    item.velocityZ = velocityZ;
    item.spawned = std::chrono::steady_clock::now();
    item.pickupAt = item.spawned + std::chrono::milliseconds(config_.itemPickupDelayMs);

    {
        std::lock_guard lock(itemsMutex_);
        if (static_cast<int>(items_.size()) >= config_.maxItemEntities) {
            return;
        }
        items_.push_back(item);
    }
    for (const auto& player : players_.snapshot()) {
        sendItemEntity(*player, item);
    }
}

void Server::dropItem(Connection& connection, const ItemStack& stack) {
    if (stack.empty()) {
        return;
    }
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double yaw = static_cast<double>(connection.yaw()) * radians;
    const double pitch = static_cast<double>(connection.pitch()) * radians;
    const double horizontal = std::cos(pitch) * 0.3;
    spawnItemEntity(stack,
                    connection.x(),
                    connection.y() + 1.3,
                    connection.z(),
                    -std::sin(yaw) * horizontal,
                    -std::sin(pitch) * 0.3 + 0.1,
                    std::cos(yaw) * horizontal);
}

void Server::dropCursor(Connection& connection, bool dropAll) {
    ItemStack cursor = connection.cursorItem();
    if (cursor.empty()) {
        return;
    }
    if (!dropAll && cursor.count > 1) {
        ItemStack one = cursor;
        one.count = 1;
        --cursor.count;
        connection.setCursorItem(cursor);
        dropItem(connection, one);
    } else {
        connection.setCursorItem(ItemStack{});
        dropItem(connection, cursor);
    }
}

void Server::sendItemEntity(Connection& connection, const ItemEntity& item) {
    connection.send(0x0E, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(item.entityId);
        buffer.writeI8(2);
        buffer.writeI32(fixedPoint(item.x));
        buffer.writeI32(fixedPoint(item.y));
        buffer.writeI32(fixedPoint(item.z));
        buffer.writeI8(0);
        buffer.writeI8(0);
        buffer.writeI32(1);
        buffer.writeI16(0);
        buffer.writeI16(0);
        buffer.writeI16(0);
    });
    sendItemMetadata(connection, item);
}

void Server::sendItemMetadata(Connection& connection, const ItemEntity& item) {
    connection.send(0x1C, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(item.entityId);
        buffer.writeU8(0xAA);
        writeItemStack(buffer, ItemStack{static_cast<i16>(item.itemId), item.count, static_cast<i16>(item.damage)});
        buffer.writeU8(0x7F);
    });
}

void Server::sendItemMetadata(const ItemEntity& item) {
    for (const auto& player : players_.snapshot()) {
        sendItemMetadata(*player, item);
    }
}

void Server::sendActiveItems(Connection& connection) {
    std::vector<ItemEntity> snapshot;
    {
        std::lock_guard lock(itemsMutex_);
        snapshot = items_;
    }
    for (const auto& item : snapshot) {
        sendItemEntity(connection, item);
    }
}

void Server::sendItemMovement(const ItemEntity& item) {
    const double deltaX = item.x - item.lastX;
    const double deltaY = item.y - item.lastY;
    const double deltaZ = item.z - item.lastZ;
    if (std::abs(deltaX) < 1.0 / 64.0 && std::abs(deltaY) < 1.0 / 64.0 && std::abs(deltaZ) < 1.0 / 64.0) {
        return;
    }
    const bool teleport = std::abs(deltaX) > 3.9 || std::abs(deltaY) > 3.9 || std::abs(deltaZ) > 3.9;
    const i8 dx = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaX * 32.0)), -128, 127));
    const i8 dy = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaY * 32.0)), -128, 127));
    const i8 dz = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaZ * 32.0)), -128, 127));
    for (const auto& player : players_.snapshot()) {
        if (teleport) {
            player->send(0x18, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(item.entityId);
                buffer.writeI32(fixedPoint(item.x));
                buffer.writeI32(fixedPoint(item.y));
                buffer.writeI32(fixedPoint(item.z));
                buffer.writeI8(0);
                buffer.writeI8(0);
                buffer.writeBool(item.resting);
            });
        } else {
            player->send(0x15, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(item.entityId);
                buffer.writeI8(dx);
                buffer.writeI8(dy);
                buffer.writeI8(dz);
                buffer.writeBool(item.resting);
            });
        }
    }
}

void Server::sendItemDestroy(std::int32_t entityId) {
    for (const auto& player : players_.snapshot()) {
        player->send(0x13, [&](ByteBuffer& buffer) {
            buffer.writeVarInt(1);
            buffer.writeVarInt(entityId);
        });
    }
}

void Server::giveStarterKit(Connection& connection) {
    std::array<ItemStack, 45> inventory{};
    inventory[36] = ItemStack{274, 1, 0};
    inventory[37] = ItemStack{275, 1, 0};
    inventory[38] = ItemStack{273, 1, 0};
    inventory[39] = ItemStack{272, 1, 0};
    inventory[40] = ItemStack{3, 64, 0};
    inventory[41] = ItemStack{4, 64, 0};
    inventory[42] = ItemStack{1, 64, 0};
    inventory[43] = ItemStack{5, 64, 0};
    inventory[44] = ItemStack{50, 64, 0};
    inventory[9] = ItemStack{260, 16, 0};
    connection.setInventoryFull(inventory, ItemStack{});
}

void Server::sendEquipment(Connection& viewer, const std::shared_ptr<Connection>& subject) {
    const ItemStack held = subject->heldItem();
    viewer.send(0x04, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(subject->entityId());
        buffer.writeI16(0);
        writeItemStack(buffer, held);
    });
}

void Server::broadcastEquipment(const std::shared_ptr<Connection>& subject) {
    if (!subject) {
        return;
    }
    for (const auto& player : players_.snapshot()) {
        if (player->entityId() != subject->entityId()) {
            sendEquipment(*player, subject);
        }
    }
}

void Server::handleHandshake(Connection& connection, std::int32_t packetId, ByteBuffer& payload) {
    if (packetId != 0x00) {
        connection.requestClose();
        return;
    }
    const i32 version = payload.readVarInt();
    const std::string host = payload.readString(255);
    payload.readU16();
    const i32 nextState = payload.readVarInt();

    connection.setProtocolVersion(version);
    connection.setVirtualHost(host);

    if (nextState == 1) {
        connection.setState(ConnectionState::Status);
    } else if (nextState == 2) {
        connection.setState(ConnectionState::Login);
    } else {
        connection.requestClose();
    }
}

void Server::handleStatus(Connection& connection, std::int32_t packetId, ByteBuffer& payload) {
    if (packetId == 0x00) {
        sendStatus(connection);
        return;
    }
    if (packetId == 0x01) {
        const i64 time = payload.readI64();
        connection.send(0x01, [&](ByteBuffer& buffer) { buffer.writeI64(time); });
        connection.closeGracefully();
        return;
    }
    connection.requestClose();
}

void Server::handleLogin(Connection& connection, std::int32_t packetId, ByteBuffer& payload) {
    if (packetId == 0x00) {
        const std::string username = payload.readString(16);
        if (!validUsername(username)) {
            connection.disconnect(chat::text("Invalid username"));
            return;
        }
        if (connection.protocolVersion() != kProtocolVersion) {
            connection.disconnect(chat::text("Outdated client! Please use 1.8.9"));
            return;
        }
        if (players_.count() >= static_cast<std::size_t>(config_.maxPlayers)) {
            Log::warn("rejected {} from {}: server full", username, connection.remoteAddress());
            connection.disconnect(chat::text("Server is full"));
            return;
        }
        if (players_.nameTaken(username) && !config_.nameReplace) {
            Log::warn("rejected {} from {}: name already online", username, connection.remoteAddress());
            connection.disconnect(chat::text("A player with this name is already online"));
            return;
        }

        connection.setPendingUsername(username);

        if (config_.onlineMode) {
            Bytes token = randomBytes(4);
            connection.setVerificationToken(token);
            const Bytes publicKey = rsaKey_->publicKeyDer();
            connection.send(0x01, [&](ByteBuffer& buffer) {
                buffer.writeString("");
                buffer.writeVarInt(static_cast<i32>(publicKey.size()));
                buffer.writeBytes(publicKey);
                buffer.writeVarInt(static_cast<i32>(token.size()));
                buffer.writeBytes(token);
            });
            return;
        }

        Profile profile;
        profile.id = Uuid::offline(username);
        profile.name = username;
        finishLogin(connection.shared_from_this(), profile);
        return;
    }

    if (packetId == 0x01) {
        if (!config_.onlineMode || rsaKey_ == nullptr) {
            connection.disconnect(chat::text("Unexpected encryption response"));
            return;
        }
        const i32 secretSize = payload.readVarInt();
        if (secretSize < 0 || secretSize > 256) {
            throw ProtocolError("invalid shared secret size");
        }
        const Bytes encryptedSecret = payload.readBytes(static_cast<std::size_t>(secretSize));
        const i32 tokenSize = payload.readVarInt();
        if (tokenSize < 0 || tokenSize > 256) {
            throw ProtocolError("invalid verify token size");
        }
        const Bytes encryptedToken = payload.readBytes(static_cast<std::size_t>(tokenSize));

        const Bytes secret = rsaKey_->decrypt(encryptedSecret);
        const Bytes token = rsaKey_->decrypt(encryptedToken);
        if (secret.size() != 16 || token != connection.verificationToken()) {
            connection.disconnect(chat::text("Invalid encryption response"));
            return;
        }

        connection.enableEncryption(secret);

        const Bytes publicKey = rsaKey_->publicKeyDer();
        const std::string hash = minecraftServerHash("", secret, publicKey);
        const std::string username = connection.pendingUsername();
        const std::string ip = config_.onlineModeIpCheck ? connection.remoteAddress() : std::string();
        const std::weak_ptr<Connection> weak = connection.weak_from_this();

        authPool_.submit([this, weak, username, hash, ip] {
            auto profileValue = AuthClient::hasJoined(username, hash, ip);
            std::shared_ptr<Profile> profile;
            if (profileValue) {
                profile = std::make_shared<Profile>(std::move(*profileValue));
            }
            if (const auto target = weak.lock()) {
                target->loop().post([this, weak, profile] {
                    const auto connection = weak.lock();
                    if (!connection || connection->dead() || connection->state() != ConnectionState::Login) {
                        return;
                    }
                    if (profile) {
                        finishLogin(connection, *profile);
                    } else {
                        connection->disconnect(chat::text("Failed to verify username"));
                    }
                });
            }
        });
        return;
    }

    connection.requestClose();
}

void Server::finishLogin(const std::shared_ptr<Connection>& connection, const Profile& profile) {
    if (!connection || connection->dead() || connection->state() != ConnectionState::Login) {
        return;
    }

    const i32 entityId = nextEntityId_.fetch_add(1);
    connection->setIdentity(profile.id, profile.name, profile.properties);
    connection->setEntityId(entityId);
    connection->setGameMode(config_.gameMode);
    connection->setPosition(static_cast<double>(world_.spawnX()) + 0.5,
                            static_cast<double>(world_.spawnY()),
                            static_cast<double>(world_.spawnZ()) + 0.5);
    connection->setRotation(0.0F, 0.0F);
    connection->setOnGround(true);
    connection->setFallHeight(static_cast<double>(world_.spawnY()));
    connection->resetSurvivalTimers();
    if (!loadPlayerData(*connection) && config_.starterKit) {
        giveStarterKit(*connection);
    }

    if (config_.compressionThreshold >= 0) {
        const int threshold = config_.compressionThreshold;
        connection->send(0x03, [&](ByteBuffer& buffer) { buffer.writeVarInt(threshold); });
        connection->enableCompression(threshold);
    }

    connection->send(0x02, [&](ByteBuffer& buffer) {
        buffer.writeString(profile.id.toString());
        buffer.writeString(profile.name);
    });
    connection->setState(ConnectionState::Play);

    if (config_.nameReplace && players_.nameTaken(profile.name)) {
        kickExistingByName(profile.name);
    }

    sendJoinSequence(*connection);

    if (!players_.add(connection, static_cast<std::size_t>(config_.maxPlayers))) {
        Log::warn("rejected {} from {}: server full or name already online",
                  profile.name, connection->remoteAddress());
        connection->disconnect(chat::text("Server is full or name already online"));
        return;
    }

    connection->setBroadcastState(connection->x(), connection->y(), connection->z(), connection->yaw(), connection->pitch());
    Log::info("{} [{}] joined from {}", profile.name, profile.id.toString(), connection->remoteAddress());

    const auto everyone = players_.snapshot();
    for (const auto& other : everyone) {
        if (other != connection) {
            sendPlayerInfoAddOne(*other, connection);
        }
    }

    broadcastPlayerSpawns(*connection);
    sendActiveItems(*connection);
    sendActiveFallingBlocks(*connection);
    sendActiveMobs(*connection);
    sendJoinMessage(*connection);
    scriptEngine_.fire("on_join", {entityId}, {profile.name});
}

void Server::sendJoinSequence(Connection& connection) {
    const int mode = connection.gameMode();
    const u8 gameModeByte = static_cast<u8>((mode & 0x07) | (config_.hardcore ? 0x08 : 0));
    const u8 maxPlayers = static_cast<u8>(std::clamp(config_.maxPlayers, 1, 255));
    const std::string levelType = config_.levelType.empty() ? "default" : config_.levelType;

    connection.send(0x01, [&](ByteBuffer& buffer) {
        buffer.writeI32(connection.entityId());
        buffer.writeU8(gameModeByte);
        buffer.writeI8(0);
        buffer.writeU8(static_cast<u8>(config_.difficulty));
        buffer.writeU8(maxPlayers);
        buffer.writeString(levelType);
        buffer.writeBool(false);
    });

    connection.send(0x05, [&](ByteBuffer& buffer) {
        buffer.writePosition(world_.spawnX(), world_.spawnY(), world_.spawnZ());
    });

    connection.send(0x41, [&](ByteBuffer& buffer) {
        buffer.writeU8(static_cast<u8>(config_.difficulty));
    });

    sendAbilities(connection);
    sendTime(connection);

    sendHealth(connection);

    connection.send(0x1F, [&](ByteBuffer& buffer) {
        buffer.writeF32(0.0F);
        buffer.writeVarInt(0);
        buffer.writeVarInt(0);
    });

    sendInitialPlayerList(connection);
    sendChunksAround(connection);
    connection.sendInventoryState();

    connection.send(0x08, [&](ByteBuffer& buffer) {
        buffer.writeF64(connection.x());
        buffer.writeF64(connection.y());
        buffer.writeF64(connection.z());
        buffer.writeF32(connection.yaw());
        buffer.writeF32(connection.pitch());
        buffer.writeI8(0);
    });

    sendSystem(connection, "&aWelcome to &f" + config_.motd);
    sendSystem(connection, "&eType /help for a list of commands");
}

void Server::sendStatus(Connection& connection) {
    Json root = Json::object();
    root["version"] = Json::object();
    root["version"]["name"] = std::string(kServerVersion);
    root["version"]["protocol"] = static_cast<std::int64_t>(kProtocolVersion);

    const auto online = players_.snapshot();
    root["players"] = Json::object();
    root["players"]["max"] = static_cast<std::int64_t>(config_.maxPlayers);
    root["players"]["online"] = static_cast<std::int64_t>(online.size());
    Json sample = Json::array();
    std::vector<std::shared_ptr<Connection>> sorted = online;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left->username() < right->username();
    });
    for (std::size_t i = 0; i < sorted.size() && i < 12; ++i) {
        Json entry = Json::object();
        entry["name"] = sorted[i]->username();
        entry["id"] = sorted[i]->uuid().toString();
        sample.push(std::move(entry));
    }
    root["players"]["sample"] = std::move(sample);
    root["description"] = Json::parse(chat::statusDescription(config_.motd));

    const std::string json = root.dump();
    connection.send(0x00, [&](ByteBuffer& buffer) { buffer.writeString(json); });
}

void Server::sendInitialPlayerList(Connection& connection) {
    auto everyone = players_.snapshot();
    const bool containsSelf = std::any_of(everyone.begin(), everyone.end(), [&](const auto& player) {
        return player->entityId() == connection.entityId();
    });
    if (!containsSelf && connection.authenticated()) {
        everyone.push_back(connection.shared_from_this());
    }
    if (!everyone.empty()) {
        sendPlayerInfoAdd(connection, everyone);
    }
}

void Server::sendPlayerInfoAdd(Connection& connection, const std::vector<std::shared_ptr<Connection>>& entries) {
    if (entries.empty()) {
        return;
    }
    ByteBuffer packet;
    packet.writeVarInt(0x38);
    packet.writeVarInt(0);
    packet.writeVarInt(static_cast<i32>(entries.size()));
    for (const auto& entry : entries) {
        const auto& uuid = entry->uuid().bytes;
        packet.writeBytes(uuid);
        packet.writeString(entry->username());
        const auto& properties = entry->properties();
        packet.writeVarInt(static_cast<i32>(properties.size()));
        for (const auto& property : properties) {
            packet.writeString(property.name);
            packet.writeString(property.value);
            const bool hasSignature = !property.signature.empty();
            packet.writeBool(hasSignature);
            if (hasSignature) {
                packet.writeString(property.signature);
            }
        }
        packet.writeVarInt(entry->gameMode());
        packet.writeVarInt(0);
        packet.writeBool(false);
    }
    connection.sendPacket(std::move(packet));
}

void Server::sendPlayerInfoAddOne(Connection& connection, const std::shared_ptr<Connection>& entry) {
    sendPlayerInfoAdd(connection, std::vector<std::shared_ptr<Connection>>{entry});
}

void Server::sendPlayerInfoRemove(Connection& connection, const Uuid& uuid) {
    connection.send(0x38, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(4);
        buffer.writeVarInt(1);
        buffer.writeBytes(uuid.bytes);
    });
}

void Server::sendPlayerInfoGameMode(Connection& connection, const std::shared_ptr<Connection>& entry) {
    connection.send(0x38, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(1);
        buffer.writeVarInt(1);
        buffer.writeBytes(entry->uuid().bytes);
        buffer.writeVarInt(entry->gameMode());
    });
}

void Server::sendChunksAround(Connection& connection) {
    const int radius = std::clamp(std::min(connection.viewDistance(), config_.viewDistance), 2, config_.viewDistance);
    const auto baseX = static_cast<i32>(std::floor(connection.x()));
    const auto baseZ = static_cast<i32>(std::floor(connection.z()));
    const i32 centerX = floorDiv(baseX, 16);
    const i32 centerZ = floorDiv(baseZ, 16);

    std::vector<std::pair<i32, i32>> coordinates;
    coordinates.reserve(static_cast<std::size_t>((radius * 2 + 1) * (radius * 2 + 1)));
    for (i32 dx = -radius; dx <= radius; ++dx) {
        for (i32 dz = -radius; dz <= radius; ++dz) {
            coordinates.emplace_back(dx, dz);
        }
    }
    std::sort(coordinates.begin(), coordinates.end(), [](const auto& left, const auto& right) {
        const auto leftDistance = left.first * left.first + left.second * left.second;
        const auto rightDistance = right.first * right.first + right.second * right.second;
        if (leftDistance != rightDistance) {
            return leftDistance < rightDistance;
        }
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    });

    for (const auto& [dx, dz] : coordinates) {
        const i32 chunkX = centerX + dx;
        const i32 chunkZ = centerZ + dz;
        const std::uint64_t high = static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunkX)) << 32;
        const std::int64_t key = static_cast<std::int64_t>(high | static_cast<std::uint32_t>(chunkZ));
        if (connection.hasSentChunk(key)) {
            continue;
        }
        sendChunkTo(connection, chunkX, chunkZ);
        connection.markChunkSent(key);
    }
}

void Server::sendChunkTo(Connection& connection, std::int32_t chunkX, std::int32_t chunkZ) {
    const auto column = world_.chunk(chunkX, chunkZ);
    const auto mesh = column->serialized();
    connection.send(0x21, [&](ByteBuffer& buffer) {
        buffer.writeI32(chunkX);
        buffer.writeI32(chunkZ);
        buffer.writeBool(true);
        buffer.writeU16(mesh->mask);
        buffer.writeVarInt(static_cast<i32>(mesh->data.size()));
        buffer.writeBytes(mesh->data);
    });
    connection.markChunkSent((static_cast<std::int64_t>(static_cast<std::uint32_t>(chunkX)) << 32)
        | static_cast<std::uint32_t>(chunkZ));
}

void Server::refreshChunkAt(std::int32_t worldX, std::int32_t worldZ) {
    const i32 chunkX = floorDiv(worldX, 16);
    const i32 chunkZ = floorDiv(worldZ, 16);
    for (const auto& player : players_.snapshot()) {
        const i32 playerChunkX = floorDiv(static_cast<i32>(std::floor(player->x())), 16);
        const i32 playerChunkZ = floorDiv(static_cast<i32>(std::floor(player->z())), 16);
        const int radius = player->viewDistance() + 1;
        if (std::abs(playerChunkX - chunkX) <= radius && std::abs(playerChunkZ - chunkZ) <= radius) {
            sendChunkTo(*player, chunkX, chunkZ);
        }
    }
}

void Server::sendAbilities(Connection& connection) {
    const int mode = connection.gameMode();
    connection.send(0x39, [&](ByteBuffer& buffer) {
        buffer.writeI8(static_cast<i8>(abilitiesFlags(mode)));
        buffer.writeF32(0.05F);
        buffer.writeF32(0.1F);
    });
}

int Server::abilitiesFlags(int mode) const {
    if (mode == 1) {
        return 0x01 | 0x04 | 0x08;
    }
    if (mode == 3) {
        return 0x01 | 0x04;
    }
    return 0;
}

void Server::sendRespawn(Connection& connection) {
    const int mode = connection.gameMode();
    const u8 gameModeByte = static_cast<u8>((mode & 0x07) | (config_.hardcore ? 0x08 : 0));
    const std::string levelType = config_.levelType.empty() ? "default" : config_.levelType;

    connection.send(0x07, [&](ByteBuffer& buffer) {
        buffer.writeI32(0);
        buffer.writeU8(static_cast<u8>(config_.difficulty));
        buffer.writeU8(gameModeByte);
        buffer.writeString(levelType);
    });

    connection.send(0x08, [&](ByteBuffer& buffer) {
        buffer.writeF64(connection.x());
        buffer.writeF64(connection.y());
        buffer.writeF64(connection.z());
        buffer.writeF32(connection.yaw());
        buffer.writeF32(connection.pitch());
        buffer.writeI8(0);
    });

    sendAbilities(connection);
    sendHealth(connection);
    connection.sendInventoryState();
    connection.setBroadcastState(connection.x(), connection.y(), connection.z(), connection.yaw(), connection.pitch());
}

void Server::sendHealth(Connection& connection) {
    connection.send(0x06, [&](ByteBuffer& buffer) {
        buffer.writeF32(connection.health());
        buffer.writeVarInt(connection.food());
        buffer.writeF32(connection.saturation());
    });
}

void Server::applyDamage(Connection& connection, float amount) {
    if (connection.playerDead() || amount <= 0.0F) {
        return;
    }
    const float newHealth = std::max(0.0F, connection.health() - amount);
    connection.setHealth(newHealth);
    sendHealth(connection);
    if (newHealth > 0.0F) {
        return;
    }

    connection.setPlayerDead(true);
    const auto inventory = connection.inventorySnapshot();
    const ItemStack cursor = connection.cursorItem();
    connection.clearInventory();
    for (const auto& stack : inventory) {
        if (!stack.empty()) {
            spawnItemEntity(stack,
                            connection.x(),
                            connection.y() + 1.0,
                            connection.z(),
                            static_cast<double>((stack.id % 5) - 2) * 0.02,
                            0.15,
                            static_cast<double>((stack.damage % 5) - 2) * 0.02);
        }
    }
    if (!cursor.empty()) {
        spawnItemEntity(cursor, connection.x(), connection.y() + 1.0, connection.z(), 0.0, 0.15, 0.0);
    }
    broadcastEquipment(connection.shared_from_this());
    Log::info("{} died", connection.username());
    scriptEngine_.fire("on_death", {connection.entityId()}, {connection.username()});
}

void Server::healPlayer(Connection& connection, float amount) {
    if (connection.playerDead()) {
        return;
    }
    connection.setHealth(connection.health() + amount);
    sendHealth(connection);
}

void Server::feedPlayer(Connection& connection, int amount) {
    connection.setFood(connection.food() + amount);
    connection.setSaturation(5.0F);
    sendHealth(connection);
}

void Server::hurtPlayer(Connection& connection, float amount) {
    applyDamage(connection, amount);
}

void Server::setFoodLevel(Connection& connection, int value) {
    connection.setFood(value);
    connection.setSaturation(0.0F);
    sendHealth(connection);
}

void Server::handleRespawn(Connection& connection) {
    if (!connection.playerDead()) {
        return;
    }
    connection.setPlayerDead(false);
    connection.setHealth(20.0F);
    connection.setFood(20);
    connection.setSaturation(5.0F);
    connection.setPosition(static_cast<double>(world_.spawnX()) + 0.5,
                           static_cast<double>(world_.spawnY()),
                           static_cast<double>(world_.spawnZ()) + 0.5);
    connection.setRotation(0.0F, 0.0F);
    connection.setOnGround(true);
    connection.setFallHeight(world_.spawnY());
    connection.resetSurvivalTimers();
    sendRespawn(connection);
    sendHealth(connection);
    connection.clearSentChunks();
    sendChunksAround(connection);
    sendActiveItems(connection);
    sendActiveFallingBlocks(connection);
    sendActiveMobs(connection);
}

void Server::updateSurvival(Connection& connection) {
    if (connection.state() != ConnectionState::Play || connection.playerDead()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& timers = connection.survivalTimers();

    const auto eyeY = static_cast<i32>(std::floor(connection.y() + 1.6));
    const auto feetY = static_cast<i32>(std::floor(connection.y()));
    const auto blockX = static_cast<i32>(std::floor(connection.x()));
    const auto blockZ = static_cast<i32>(std::floor(connection.z()));
    const u16 eyeBlock = world_.getBlock(blockX, eyeY, blockZ);
    const u16 feetBlock = world_.getBlock(blockX, feetY, blockZ);
    const u16 eyeId = static_cast<u16>(eyeBlock >> 4);
    const u16 feetId = static_cast<u16>(feetBlock >> 4);

    if (eyeId == 8 || eyeId == 9) {
        ++timers.waterTicks;
        if (timers.waterTicks > 300 && now - timers.lastDrownDamage > std::chrono::seconds(1)) {
            timers.lastDrownDamage = now;
            applyDamage(connection, 2.0F);
        }
    } else {
        timers.waterTicks = 0;
    }

    if ((feetId == 10 || feetId == 11) && now - timers.lastLavaDamage > std::chrono::milliseconds(500)) {
        timers.lastLavaDamage = now;
        applyDamage(connection, 4.0F);
    }

    if (now - timers.lastHunger > std::chrono::seconds(config_.hungerIntervalSeconds)) {
        timers.lastHunger = now;
        if (connection.food() > 0) {
            connection.setFood(connection.food() - 1);
            sendHealth(connection);
        }
    }

    if (connection.food() >= 18 && connection.health() < 20.0F
        && now - timers.lastRegen > std::chrono::seconds(4)) {
        timers.lastRegen = now;
        connection.setHealth(connection.health() + 1.0F);
        sendHealth(connection);
    }

    if (connection.food() == 0 && connection.health() > 0.0F
        && now - timers.lastStarve > std::chrono::seconds(4)) {
        timers.lastStarve = now;
        applyDamage(connection, 1.0F);
    }
}

void Server::sendTime(Connection& connection) {
    const i64 age = world_.gameAge();
    const i64 time = world_.timeOfDay();
    connection.send(0x03, [&](ByteBuffer& buffer) {
        buffer.writeI64(age);
        buffer.writeI64(time);
    });
    connection.setLastTimeUpdate(std::chrono::steady_clock::now());
}

void Server::broadcastTime() {
    for (const auto& player : players_.snapshot()) {
        sendTime(*player);
    }
}

void Server::sendPlayerSpawn(Connection& viewer, const std::shared_ptr<Connection>& subject) {
    viewer.send(0x0C, [&](ByteBuffer& buffer) {
        buffer.writeVarInt(subject->entityId());
        buffer.writeBytes(subject->uuid().bytes);
        buffer.writeI32(fixedPoint(subject->x()));
        buffer.writeI32(fixedPoint(subject->y()));
        buffer.writeI32(fixedPoint(subject->z()));
        buffer.writeI8(angleByte(subject->yaw()));
        buffer.writeI8(angleByte(subject->pitch()));
        buffer.writeI16(0);
        buffer.writeU8(0x0A);
        buffer.writeU8(0x7F);
        buffer.writeU8(0x7F);
    });
}

void Server::broadcastPlayerSpawns(Connection& joining) {
    for (const auto& other : players_.snapshot()) {
        if (other->entityId() == joining.entityId()) {
            continue;
        }
        sendPlayerSpawn(joining, other);
        sendPlayerSpawn(*other, joining.shared_from_this());
        sendEquipment(joining, other);
        sendEquipment(*other, joining.shared_from_this());
    }
}

void Server::broadcastEntityTeleport(const std::shared_ptr<Connection>& subject) {
    if (!subject) {
        return;
    }
    for (const auto& viewer : players_.snapshot()) {
        if (viewer->entityId() == subject->entityId()) {
            continue;
        }
        viewer->send(0x18, [&](ByteBuffer& buffer) {
            buffer.writeVarInt(subject->entityId());
            buffer.writeI32(fixedPoint(subject->x()));
            buffer.writeI32(fixedPoint(subject->y()));
            buffer.writeI32(fixedPoint(subject->z()));
            buffer.writeI8(angleByte(subject->yaw()));
            buffer.writeI8(angleByte(subject->pitch()));
            buffer.writeBool(subject->onGround());
        });
    }
    subject->setBroadcastState(subject->x(), subject->y(), subject->z(), subject->yaw(), subject->pitch());
}

void Server::broadcastEntityMovement(const std::shared_ptr<Connection>& subject) {
    if (!subject) {
        return;
    }
    const double x = subject->x();
    const double y = subject->y();
    const double z = subject->z();
    const float yaw = subject->yaw();
    const float pitch = subject->pitch();
    if (!subject->hasBroadcastState()) {
        subject->setBroadcastState(x, y, z, yaw, pitch);
        return;
    }

    const double deltaX = x - subject->broadcastX();
    const double deltaY = y - subject->broadcastY();
    const double deltaZ = z - subject->broadcastZ();
    const bool teleport = std::abs(deltaX) > 3.9 || std::abs(deltaY) > 3.9 || std::abs(deltaZ) > 3.9;

    const i8 dx = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaX * 32.0)), -128, 127));
    const i8 dy = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaY * 32.0)), -128, 127));
    const i8 dz = static_cast<i8>(std::clamp(static_cast<int>(std::lround(deltaZ * 32.0)), -128, 127));
    const bool moved = dx != 0 || dy != 0 || dz != 0;
    const i8 yawByte = angleByte(yaw);
    const i8 pitchByte = angleByte(pitch);
    const bool rotated = yawByte != angleByte(subject->broadcastYaw()) || pitchByte != angleByte(subject->broadcastPitch());

    if (!teleport && !moved && !rotated) {
        return;
    }

    for (const auto& viewer : players_.snapshot()) {
        if (viewer->entityId() == subject->entityId()) {
            continue;
        }
        if (teleport) {
            viewer->send(0x18, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(subject->entityId());
                buffer.writeI32(fixedPoint(x));
                buffer.writeI32(fixedPoint(y));
                buffer.writeI32(fixedPoint(z));
                buffer.writeI8(yawByte);
                buffer.writeI8(pitchByte);
                buffer.writeBool(subject->onGround());
            });
        } else if (moved && rotated) {
            viewer->send(0x17, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(subject->entityId());
                buffer.writeI8(dx);
                buffer.writeI8(dy);
                buffer.writeI8(dz);
                buffer.writeI8(yawByte);
                buffer.writeI8(pitchByte);
                buffer.writeBool(subject->onGround());
            });
        } else if (moved) {
            viewer->send(0x15, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(subject->entityId());
                buffer.writeI8(dx);
                buffer.writeI8(dy);
                buffer.writeI8(dz);
                buffer.writeBool(subject->onGround());
            });
        } else {
            viewer->send(0x16, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(subject->entityId());
                buffer.writeI8(yawByte);
                buffer.writeI8(pitchByte);
                buffer.writeBool(subject->onGround());
            });
        }
        if (rotated) {
            viewer->send(0x19, [&](ByteBuffer& buffer) {
                buffer.writeVarInt(subject->entityId());
                buffer.writeI8(yawByte);
            });
        }
    }
    subject->setBroadcastState(x, y, z, yaw, pitch);
}

void Server::broadcastEntityDestroy(std::int32_t entityId) {
    for (const auto& viewer : players_.snapshot()) {
        viewer->send(0x13, [&](ByteBuffer& buffer) {
            buffer.writeVarInt(1);
            buffer.writeVarInt(entityId);
        });
    }
}

void Server::sendSystem(Connection& connection, const std::string& message) {
    sendChat(connection, message, 1);
}

void Server::sendChat(Connection& connection, const std::string& message, std::int8_t position) {
    const std::string json = chat::legacy(message);
    connection.send(0x02, [&](ByteBuffer& buffer) {
        buffer.writeString(json);
        buffer.writeI8(position);
    });
}

void Server::broadcastSystem(const std::string& message) {
    const std::string json = chat::legacy(message);
    for (const auto& player : players_.snapshot()) {
        player->send(0x02, [&](ByteBuffer& buffer) {
            buffer.writeString(json);
            buffer.writeI8(1);
        });
    }
    Log::info("{}", chat::plain(message));
}

void Server::broadcastChat(const std::string& message) {
    const std::string json = chat::legacy(message);
    for (const auto& player : players_.snapshot()) {
        player->send(0x02, [&](ByteBuffer& buffer) {
            buffer.writeString(json);
            buffer.writeI8(0);
        });
    }
}

void Server::broadcastPlayerChat(Connection& source, const std::string& message) {
    Log::info("<{}> {}", source.username(), chat::plain(message));
    const std::string json = chat::playerMessage(source.username(), message);
    for (const auto& player : players_.snapshot()) {
        player->send(0x02, [&](ByteBuffer& buffer) {
            buffer.writeString(json);
            buffer.writeI8(0);
        });
    }
}

void Server::sendJoinMessage(Connection& connection) {
    broadcastSystem("&e" + connection.username() + " joined the game");
}

void Server::sendLeaveMessage(const std::string& username) {
    broadcastSystem("&e" + username + " left the game");
}

void Server::setGameMode(const std::shared_ptr<Connection>& connection, int mode) {
    if (!connection) {
        return;
    }
    const int clamped = std::clamp(mode, 0, 3);
    connection->setGameMode(clamped);
    for (const auto& player : players_.snapshot()) {
        sendPlayerInfoGameMode(*player, connection);
    }

    const auto refresh = [this, connection] {
        if (!connection || connection->dead() || connection->state() != ConnectionState::Play) {
            return;
        }
        sendRespawn(*connection);
        connection->clearSentChunks();
        sendChunksAround(*connection);
    };
    if (connection->loop().isLoopThread()) {
        refresh();
    } else {
        connection->loop().post(refresh);
    }

    Log::info("{} changed game mode to {}", connection->username(), gameModeName(clamped));
}

void Server::teleport(const std::shared_ptr<Connection>& connection, double x, double y, double z) {
    if (!connection || !validMovement(x, y, z)) {
        return;
    }
    const double oldX = connection->x();
    const double oldZ = connection->z();
    connection->setTeleportTarget(x, y, z, connection->yaw(), connection->pitch());
    connection->setFallHeight(y);
    connection->send(0x08, [&](ByteBuffer& buffer) {
        buffer.writeF64(x);
        buffer.writeF64(y);
        buffer.writeF64(z);
        buffer.writeF32(connection->yaw());
        buffer.writeF32(connection->pitch());
        buffer.writeI8(0);
    });
    if (floorDiv(static_cast<i32>(std::floor(oldX)), 16) != floorDiv(static_cast<i32>(std::floor(x)), 16)
        || floorDiv(static_cast<i32>(std::floor(oldZ)), 16) != floorDiv(static_cast<i32>(std::floor(z)), 16)) {
        sendChunksAround(*connection);
    }
    broadcastEntityTeleport(connection);
}

void Server::kick(const std::shared_ptr<Connection>& connection, const std::string& reason) {
    if (!connection) {
        return;
    }
    Log::info("kicking {}: {}", connection->username(), reason);
    connection->disconnect(chat::text(reason));
}

void Server::broadcastBlockChange(std::int32_t x, std::int32_t y, std::int32_t z, u16 block) {
    for (const auto& player : players_.snapshot()) {
        player->send(0x23, [&](ByteBuffer& buffer) {
            buffer.writePosition(x, y, z);
            buffer.writeVarInt(static_cast<i32>(block));
        });
    }
}

void Server::handlePlay(Connection& connection, std::int32_t packetId, ByteBuffer& payload) {
    switch (packetId) {
    case 0x00: {
        const i32 keepAlive = payload.readVarInt();
        connection.acknowledgeKeepAlive(keepAlive);
        break;
    }
    case 0x01: {
        const std::string message = payload.readString(256);
        if (message.empty()) {
            break;
        }
        if (message.size() > 100) {
            sendSystem(connection, "Message is too long");
            break;
        }
        if (message.front() == '/') {
            commands_.execute(connection.shared_from_this(), message);
        } else {
            if (!connection.takeChatSlot(config_.chatRateLimitMs)) {
                sendSystem(connection, "You are chatting too fast");
                break;
            }
            broadcastPlayerChat(connection, message);
            scriptEngine_.fire("on_chat", {connection.entityId()}, {connection.username(), message});
        }
        break;
    }
    case 0x02: {
        const i32 target = payload.readVarInt();
        const i32 mouse = payload.readVarInt();
        if (mouse == 1) {
            attackMob(connection, target);
        } else if (mouse == 2) {
            payload.readF32();
            payload.readF32();
            payload.readF32();
        }
        break;
    }
    case 0x03: {
        connection.setOnGround(payload.readBool());
        break;
    }
    case 0x04: {
        const double x = payload.readF64();
        const double y = payload.readF64();
        const double z = payload.readF64();
        const bool onGround = payload.readBool();
        if (!validMovement(x, y, z)) {
            connection.disconnect(chat::text("Invalid movement"));
            break;
        }
        if (y < -64.0) {
            teleport(connection.shared_from_this(),
                     static_cast<double>(world_.spawnX()) + 0.5,
                     static_cast<double>(world_.spawnY()),
                     static_cast<double>(world_.spawnZ()) + 0.5);
            break;
        }
        const bool wasOnGround = connection.onGround();
        const i32 oldChunkX = floorDiv(static_cast<i32>(std::floor(connection.x())), 16);
        const i32 oldChunkZ = floorDiv(static_cast<i32>(std::floor(connection.z())), 16);
        connection.setPosition(x, y, z);
        connection.setOnGround(onGround);
        if (wasOnGround && !onGround) {
            connection.setFallHeight(y);
        } else if (!wasOnGround && onGround) {
            const double fall = connection.fallHeight() - y;
            const auto feetId = static_cast<u16>(world_.getBlock(static_cast<i32>(std::floor(x)),
                                                                 static_cast<i32>(std::floor(y)),
                                                                 static_cast<i32>(std::floor(z))) >> 4);
            if (fall > 3.0 && feetId != 8 && feetId != 9) {
                applyDamage(connection, static_cast<float>(std::floor(fall - 3.0)));
            }
            connection.setFallHeight(y);
        } else if (!onGround && y > connection.fallHeight()) {
            connection.setFallHeight(y);
        }
        if (oldChunkX != floorDiv(static_cast<i32>(std::floor(x)), 16)
            || oldChunkZ != floorDiv(static_cast<i32>(std::floor(z)), 16)) {
            sendChunksAround(connection);
        }
        if (connection.takeMovementBroadcastSlot()) {
            broadcastEntityMovement(connection.shared_from_this());
        }
        break;
    }
    case 0x05: {
        connection.setRotation(payload.readF32(), payload.readF32());
        connection.setOnGround(payload.readBool());
        if (connection.takeMovementBroadcastSlot()) {
            broadcastEntityMovement(connection.shared_from_this());
        }
        break;
    }
    case 0x06: {
        const double x = payload.readF64();
        const double y = payload.readF64();
        const double z = payload.readF64();
        const float yaw = payload.readF32();
        const float pitch = payload.readF32();
        const bool onGround = payload.readBool();
        if (!validMovement(x, y, z)) {
            connection.disconnect(chat::text("Invalid movement"));
            break;
        }
        if (y < -64.0) {
            teleport(connection.shared_from_this(),
                     static_cast<double>(world_.spawnX()) + 0.5,
                     static_cast<double>(world_.spawnY()),
                     static_cast<double>(world_.spawnZ()) + 0.5);
            break;
        }
        const bool wasOnGround = connection.onGround();
        const i32 oldChunkX = floorDiv(static_cast<i32>(std::floor(connection.x())), 16);
        const i32 oldChunkZ = floorDiv(static_cast<i32>(std::floor(connection.z())), 16);
        connection.setPosition(x, y, z);
        connection.setRotation(yaw, pitch);
        connection.setOnGround(onGround);
        if (wasOnGround && !onGround) {
            connection.setFallHeight(y);
        } else if (!wasOnGround && onGround) {
            const double fall = connection.fallHeight() - y;
            const auto feetId = static_cast<u16>(world_.getBlock(static_cast<i32>(std::floor(x)),
                                                                 static_cast<i32>(std::floor(y)),
                                                                 static_cast<i32>(std::floor(z))) >> 4);
            if (fall > 3.0 && feetId != 8 && feetId != 9) {
                applyDamage(connection, static_cast<float>(std::floor(fall - 3.0)));
            }
            connection.setFallHeight(y);
        } else if (!onGround && y > connection.fallHeight()) {
            connection.setFallHeight(y);
        }
        if (oldChunkX != floorDiv(static_cast<i32>(std::floor(x)), 16)
            || oldChunkZ != floorDiv(static_cast<i32>(std::floor(z)), 16)) {
            sendChunksAround(connection);
        }
        if (connection.takeMovementBroadcastSlot()) {
            broadcastEntityMovement(connection.shared_from_this());
        }
        break;
    }
    case 0x07: {
        const i32 status = payload.readVarInt();
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
        payload.readPosition(x, y, z);
        payload.readI8();
        if (status == 3 || status == 4) {
            if (connection.gameMode() == 3) {
                break;
            }
            const int amount = status == 4 ? 1 : -1;
            const ItemStack dropped = connection.takeHeldItem(amount);
            if (!dropped.empty()) {
                dropItem(connection, dropped);
                broadcastEquipment(connection.shared_from_this());
            }
            break;
        }
        const int gameMode = connection.gameMode();
        const bool instant = (gameMode == 0 || gameMode == 1)
            && (status == 2 || (status == 0 && gameMode == 1));
        if (instant) {
            const u16 existing = world_.getBlock(x, y, z);
            if (existing != kAir && existing != kBedrock) {
                world_.setBlock(x, y, z, kAir);
                broadcastBlockChange(x, y, z, kAir);
                scheduleFluidNeighbors(x, y, z);
                if (blockLightEmission(static_cast<u16>(existing >> 4)) > 0) {
                    refreshChunkAt(x, z);
                }
                if (connection.gameMode() != 1) {
                    spawnItemDrop(existing, x, y, z);
                }
                tryStartFalling(x, y + 1, z);
                scriptEngine_.fire("on_block_break", {x, y, z, static_cast<std::int64_t>(existing)});
            }
        }
        break;
    }
    case 0x08: {
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
        payload.readPosition(x, y, z);
        const i8 face = payload.readI8();
        const i16 packetItemId = payload.readI16();
        i16 packetDamage = 0;
        if (packetItemId != -1) {
            payload.readI8();
            packetDamage = payload.readI16();
        }
        const int mode = connection.gameMode();
        if (mode == 2 || mode == 3) {
            break;
        }

        if (face == -1) {
            const ItemStack useItem = connection.heldItem();
            if (consumeFood(connection, useItem)) {
                if (mode != 1) {
                    connection.takeHeldItem(1);
                    broadcastEquipment(connection.shared_from_this());
                }
            }
            break;
        }

        if (face < 0 || face > 5) {
            break;
        }

        ItemStack held;
        if (mode == 1) {
            if (packetItemId < 0 || !isValidItemId(packetItemId)) {
                break;
            }
            held = ItemStack{packetItemId, 1, packetDamage};
        } else {
            held = connection.heldItem();
            if (held.empty()) {
                break;
            }
        }

        if (held.id == 326 || held.id == 327) {
            useBucket(connection, held, x, y, z, face);
            break;
        }

        const u16 block = resolvePlacementState(static_cast<u16>(held.id),
                                                 static_cast<u16>(held.damage),
                                                 face,
                                                 connection.yaw());
        if (block == 0) {
            break;
        }
        static constexpr i32 offsets[6][3] = {
            {0, -1, 0},
            {0, 1, 0},
            {0, 0, -1},
            {0, 0, 1},
            {-1, 0, 0},
            {1, 0, 0},
        };
        const i32 targetX = x + offsets[static_cast<std::size_t>(face)][0];
        const i32 targetY = y + offsets[static_cast<std::size_t>(face)][1];
        const i32 targetZ = z + offsets[static_cast<std::size_t>(face)][2];
        if (targetY < 0 || targetY > 255) {
            break;
        }
        const u16 existing = world_.getBlock(targetX, targetY, targetZ);
        if (existing != kAir && existing != makeBlock(31) && existing != makeBlock(78)) {
            break;
        }
        world_.setBlock(targetX, targetY, targetZ, block);
        broadcastBlockChange(targetX, targetY, targetZ, block);
        scheduleFluidNeighbors(targetX, targetY, targetZ);
        tryGrowGrassAt(targetX, targetY, targetZ);
        if (blockLightEmission(static_cast<u16>(existing >> 4)) > 0
            || blockLightEmission(static_cast<u16>(block >> 4)) > 0) {
            refreshChunkAt(targetX, targetZ);
        }
        tryStartFalling(targetX, targetY, targetZ);
        scriptEngine_.fire("on_block_place", {targetX, targetY, targetZ, static_cast<std::int64_t>(block)});
        if (mode != 1) {
            connection.takeHeldItem(1);
            broadcastEquipment(connection.shared_from_this());
        }
        break;
    }
    case 0x09: {
        const i16 slot = payload.readI16();
        if (slot >= 0 && slot <= 8) {
            connection.setHeldSlot(slot);
            broadcastEquipment(connection.shared_from_this());
        }
        break;
    }
    case 0x0E: {
        const u8 windowId = payload.readU8();
        const i16 slot = payload.readI16();
        const i8 button = payload.readI8();
        const i16 action = payload.readI16();
        const i8 mode = payload.readI8();
        if (windowId == 0) {
            const ItemStack cursor = connection.cursorItem();
            if (slot == -999 && (mode == 0 || mode == 1) && (button == 0 || button == 1)) {
                dropCursor(connection, button == 0);
            } else if (mode == 0) {
                if (slot >= 0 && slot < 45) {
                    connection.clickSlot(slot, button);
                }
            } else if (mode == 1) {
                connection.shiftMove(slot);
            } else if (mode == 2) {
                connection.swapHotbar(slot, button);
            } else if (mode == 3) {
                if (connection.gameMode() == 1 && cursor.empty()) {
                    connection.cloneToCursor(slot);
                }
            } else if (mode == 4) {
                if (cursor.empty() && slot >= 0 && slot < 45) {
                    const ItemStack dropped = connection.takeSlotItem(slot, button == 1 ? -1 : 1);
                    if (!dropped.empty()) {
                        dropItem(connection, dropped);
                    }
                }
            } else {
                connection.sendInventoryState();
            }
            broadcastEquipment(connection.shared_from_this());
            connection.send(0x32, [&](ByteBuffer& buffer) {
                buffer.writeU8(windowId);
                buffer.writeI16(action);
                buffer.writeBool(true);
            });
        }
        break;
    }
    case 0x0D: {
        payload.readU8();
        dropCursor(connection, true);
        break;
    }
    case 0x0F: {
        payload.readU8();
        payload.readI16();
        payload.readBool();
        break;
    }
    case 0x10: {
        const i16 slot = payload.readI16();
        const ItemStack item = readItemStack(payload);
        if (connection.gameMode() == 1 && (item.empty() || isValidItemId(item.id))) {
            if (slot < 0) {
                if (!item.empty()) {
                    dropItem(connection, item);
                }
            } else if (slot >= 1 && slot < 45) {
                connection.setInventorySlot(slot, item);
            }
            broadcastEquipment(connection.shared_from_this());
        }
        break;
    }
    case 0x0A: {
        break;
    }
    case 0x0B: {
        break;
    }
    case 0x15: {
        payload.readString(16);
        const int clientView = payload.readI8();
        payload.readI8();
        payload.readBool();
        payload.readU8();
        const int oldView = connection.viewDistance();
        const int clamped = std::clamp(clientView, 2, config_.viewDistance);
        connection.setClientViewDistance(clientView);
        connection.setViewDistance(clamped);
        if (clamped > oldView) {
            sendChunksAround(connection);
        }
        break;
    }
    case 0x16: {
        const i32 action = payload.readVarInt();
        if (action == 0 && connection.playerDead()) {
            handleRespawn(connection);
        }
        break;
    }
    case 0x17: {
        const std::string channel = payload.readString(64);
        payload.readRemaining();
        if (channel == "MC|Brand") {
            const std::string brand = "minicraft-cpp";
            connection.send(0x3F, [&](ByteBuffer& buffer) {
                buffer.writeString("MC|Brand");
                buffer.writeString(brand);
            });
        }
        break;
    }
    default:
        Log::trace("unhandled play packet 0x{:02x} from {}", packetId, connection.username());
        break;
    }
}

}
