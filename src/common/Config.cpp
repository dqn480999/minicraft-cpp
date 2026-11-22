#include "common/Config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "common/Log.hpp"

namespace mc {
namespace {

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseBool(const std::string& value, bool fallback) {
    const auto v = lower(trim(value));
    if (v == "true" || v == "on" || v == "yes" || v == "1") {
        return true;
    }
    if (v == "false" || v == "off" || v == "no" || v == "0") {
        return false;
    }
    return fallback;
}

int parseInt(const std::string& value, int fallback) {
    const auto v = trim(value);
    int result = fallback;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), result);
    if (ec != std::errc{} || ptr != v.data() + v.size()) {
        return fallback;
    }
    return result;
}

double parseDoubleValue(const std::string& value, double fallback) {
    const auto v = trim(value);
    char* end = nullptr;
    const double result = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') {
        return fallback;
    }
    return result;
}

std::uint64_t parseSeed(const std::string& value) {
    const auto v = trim(value);
    std::uint64_t result = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), result);
    if (ec == std::errc{} && ptr == v.data() + v.size()) {
        return result;
    }
    std::int64_t signedResult = 0;
    const auto [sptr, sec] = std::from_chars(v.data(), v.data() + v.size(), signedResult);
    if (sec == std::errc{} && sptr == v.data() + v.size()) {
        return static_cast<std::uint64_t>(signedResult);
    }
    return 0;
}

int clampInt(int value, int low, int high) {
    return std::clamp(value, low, high);
}

}

ServerConfig Config::defaults() {
    return ServerConfig{};
}

bool Config::applyLine(ServerConfig& config, std::string_view line) {
    std::string text = trim(line);
    if (text.empty() || text.front() == '#' || text.front() == '!') {
        return false;
    }
    const auto separator = text.find('=');
    if (separator == std::string::npos) {
        return false;
    }
    const auto key = lower(trim(std::string_view(text).substr(0, separator)));
    const auto value = trim(std::string_view(text).substr(separator + 1));

    if (key == "host") {
        config.host = value;
    } else if (key == "port") {
        config.port = static_cast<std::uint16_t>(clampInt(parseInt(value, config.port), 1, 65535));
    } else if (key == "online-mode") {
        config.onlineMode = parseBool(value, config.onlineMode);
    } else if (key == "online-mode-ip-check") {
        config.onlineModeIpCheck = parseBool(value, config.onlineModeIpCheck);
    } else if (key == "name-replace") {
        config.nameReplace = parseBool(value, config.nameReplace);
    } else if (key == "max-players") {
        config.maxPlayers = clampInt(parseInt(value, config.maxPlayers), 1, 10000);
    } else if (key == "motd") {
        config.motd = value;
    } else if (key == "view-distance") {
        config.viewDistance = clampInt(parseInt(value, config.viewDistance), 2, 32);
    } else if (key == "difficulty") {
        config.difficulty = clampInt(parseInt(value, config.difficulty), 0, 3);
    } else if (key == "gamemode") {
        config.gameMode = clampInt(parseInt(value, config.gameMode), 0, 3);
    } else if (key == "hardcore") {
        config.hardcore = parseBool(value, config.hardcore);
    } else if (key == "compression-threshold") {
        config.compressionThreshold = clampInt(parseInt(value, config.compressionThreshold), -1, 1048576);
    } else if (key == "io-threads") {
        config.ioThreads = clampInt(parseInt(value, config.ioThreads), 0, 64);
    } else if (key == "pin-io-threads") {
        config.pinIoThreads = parseBool(value, config.pinIoThreads);
    } else if (key == "player-timeout-ms") {
        config.playerTimeoutMs = clampInt(parseInt(value, config.playerTimeoutMs), 1000, 600000);
    } else if (key == "max-packet-size") {
        const int parsed = parseInt(value, static_cast<int>(config.maxPacketSize));
        config.maxPacketSize = static_cast<std::size_t>(clampInt(parsed, 1024, 64 * 1024 * 1024));
    } else if (key == "max-output-buffer") {
        const int parsed = parseInt(value, static_cast<int>(config.maxOutputBuffer));
        config.maxOutputBuffer = static_cast<std::size_t>(clampInt(parsed, 65536, 256 * 1024 * 1024));
    } else if (key == "max-item-entities") {
        config.maxItemEntities = clampInt(parseInt(value, config.maxItemEntities), 16, 100000);
    } else if (key == "max-falling-blocks") {
        config.maxFallingBlocks = clampInt(parseInt(value, config.maxFallingBlocks), 16, 100000);
    } else if (key == "max-mobs") {
        config.maxMobs = clampInt(parseInt(value, config.maxMobs), 1, 10000);
    } else if (key == "level-type") {
        config.levelType = value;
    } else if (key == "level-seed") {
        config.levelSeed = parseSeed(value);
    } else if (key == "ops") {
        config.ops = value;
    } else if (key == "ops-file") {
        config.opsFile = value;
    } else if (key == "world-save") {
        config.worldSave = parseBool(value, config.worldSave);
    } else if (key == "world-dir") {
        config.worldDir = value;
    } else if (key == "item-drops") {
        config.itemDrops = parseBool(value, config.itemDrops);
    } else if (key == "starter-kit") {
        config.starterKit = parseBool(value, config.starterKit);
    } else if (key == "item-pickup-delay-ms") {
        config.itemPickupDelayMs = clampInt(parseInt(value, config.itemPickupDelayMs), 0, 600000);
    } else if (key == "item-pickup-radius") {
        config.itemPickupRadius = std::clamp(parseDoubleValue(value, config.itemPickupRadius), 0.25, 8.0);
    } else if (key == "item-pickup-animation") {
        config.itemPickupAnimation = parseBool(value, config.itemPickupAnimation);
    } else if (key == "sand-physics") {
        config.sandPhysics = parseBool(value, config.sandPhysics);
    } else if (key == "fluids-enabled") {
        config.fluidsEnabled = parseBool(value, config.fluidsEnabled);
    } else if (key == "fluid-updates-per-tick") {
        config.fluidUpdatesPerTick = clampInt(parseInt(value, config.fluidUpdatesPerTick), 1, 4096);
    } else if (key == "fluid-tick-interval") {
        config.fluidTickInterval = clampInt(parseInt(value, config.fluidTickInterval), 1, 100);
    } else if (key == "hunger-interval-seconds") {
        config.hungerIntervalSeconds = clampInt(parseInt(value, config.hungerIntervalSeconds), 1, 3600);
    } else if (key == "time-scale") {
        config.timeScale = std::clamp(parseDoubleValue(value, config.timeScale), 0.05, 100.0);
    } else if (key == "item-despawn-seconds") {
        config.itemDespawnSeconds = clampInt(parseInt(value, config.itemDespawnSeconds), 0, 86400);
    } else if (key == "autosave-seconds") {
        config.autosaveSeconds = clampInt(parseInt(value, config.autosaveSeconds), 0, 86400);
    } else if (key == "max-cached-chunks") {
        config.maxCachedChunks = clampInt(parseInt(value, config.maxCachedChunks), 64, 100000);
    } else if (key == "player-data") {
        config.playerData = parseBool(value, config.playerData);
    } else if (key == "player-data-dir") {
        config.playerDataDir = value;
    } else if (key == "chat-rate-limit-ms") {
        config.chatRateLimitMs = clampInt(parseInt(value, config.chatRateLimitMs), 0, 10000);
    } else if (key == "max-packets-per-second") {
        config.maxPacketsPerSecond = clampInt(parseInt(value, config.maxPacketsPerSecond), 0, 1000000);
    } else if (key == "legacy-ping") {
        config.legacyPing = parseBool(value, config.legacyPing);
    } else if (key == "scripts-enabled") {
        config.scriptsEnabled = parseBool(value, config.scriptsEnabled);
    } else if (key == "script-dir") {
        config.scriptDir = value;
    } else if (key == "script-tick-interval-ms") {
        config.scriptTickIntervalMs = clampInt(parseInt(value, config.scriptTickIntervalMs), 100, 3600000);
    } else if (key == "script-max-instructions") {
        config.scriptMaxInstructions = clampInt(parseInt(value, config.scriptMaxInstructions), 1000, 10000000);
    } else if (key == "script-trace") {
        config.scriptTrace = parseBool(value, config.scriptTrace);
    } else if (key == "script-disassemble") {
        config.scriptDisassemble = parseBool(value, config.scriptDisassemble);
    } else if (key == "mods-enabled") {
        config.modsEnabled = parseBool(value, config.modsEnabled);
    } else if (key == "mod-dir") {
        config.modDir = value;
    } else if (key == "debug") {
        config.debug = parseBool(value, config.debug);
    } else if (key == "color") {
        config.color = parseBool(value, config.color);
    } else if (key == "listener-name") {
        config.listenerName = value;
    } else if (key == "log-file") {
        config.logFile = value;
    } else {
        return false;
    }
    return true;
}

ServerConfig Config::load(const std::string& path) {
    ServerConfig config = defaults();
    std::ifstream input(path);
    if (!input) {
        Log::warn("config not found at {}, using defaults", path);
        return config;
    }
    std::string line;
    int applied = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (applyLine(config, line)) {
            ++applied;
        }
    }
    Log::info("loaded {} settings from {}", applied, path);
    return config;
}

}
