#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mod/ModApi.h"
#include "mod/ModValue.hpp"

namespace mc {

class Server;

class ModManager {
public:
    explicit ModManager(Server& server);
    ~ModManager();

    ModManager(const ModManager&) = delete;
    ModManager& operator=(const ModManager&) = delete;

    bool loadDirectory(const std::string& directory);
    void start();
    void stop();
    void shutdown();
    void fire(const std::string& event, const std::vector<ModValue>& arguments = {});
    bool executeCommand(std::string_view sender,
                        std::int32_t entityId,
                        bool isOp,
                        std::string_view name,
                        std::string_view arguments);
    std::vector<std::string> list() const;
    std::size_t count() const;

private:
    struct LoadedMod {
        void* handle{nullptr};
        std::string path;
        std::string name;
        std::string version;
        McModStartFn start{nullptr};
        McModStopFn stop{nullptr};
        McModShutdownFn shutdown{nullptr};
    };

    struct EventBinding {
        std::uint64_t id{0};
        std::string event;
        McModEventCallback callback{nullptr};
        void* user{nullptr};
        void* handle{nullptr};
    };

    struct CommandBinding {
        std::string name;
        std::string description;
        McModCommandCallback callback{nullptr};
        void* user{nullptr};
        void* handle{nullptr};
    };

    static void hostLog(void* host, std::int32_t level, const char* message);
    static void hostBroadcast(void* host, const char* message);
    static std::int64_t hostPlayerCount(void* host);
    static std::int32_t hostIsOp(void* host, const char* name);
    static std::int64_t hostGetBlock(void* host, std::int32_t x, std::int32_t y, std::int32_t z);
    static void hostSetBlock(void* host, std::int32_t x, std::int32_t y, std::int32_t z, std::uint16_t state);
    static std::uint64_t hostRegisterEvent(void* host,
                                           const char* event,
                                           McModEventCallback callback,
                                           void* user);
    static void hostUnregisterEvent(void* host, std::uint64_t id);
    static std::uint64_t hostRegisterCommand(void* host,
                                             const char* name,
                                             const char* description,
                                             McModCommandCallback callback,
                                             void* user);

    void removeBindings(void* handle);
    static std::string normalizeName(std::string_view value);

    Server& server_;
    mutable std::recursive_mutex mutex_;
    std::vector<LoadedMod> mods_;
    std::vector<EventBinding> events_;
    std::vector<CommandBinding> commands_;
    McModApi api_{};
    void* currentHandle_{nullptr};
    std::uint64_t nextId_{1};
    bool active_{false};
};

}
