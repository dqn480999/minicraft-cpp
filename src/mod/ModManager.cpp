#include "mod/ModManager.hpp"

#include <algorithm>
#include <cctype>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "common/Log.hpp"
#include "server/Server.hpp"

namespace mc {
namespace {

std::string lowerCopy(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

}

ModManager::ModManager(Server& server)
    : server_(server) {
    api_.abi_version = MC_MOD_ABI_VERSION;
    api_.host = this;
    api_.log = &ModManager::hostLog;
    api_.broadcast = &ModManager::hostBroadcast;
    api_.player_count = &ModManager::hostPlayerCount;
    api_.is_op = &ModManager::hostIsOp;
    api_.get_block = &ModManager::hostGetBlock;
    api_.set_block = &ModManager::hostSetBlock;
    api_.register_event = &ModManager::hostRegisterEvent;
    api_.unregister_event = &ModManager::hostUnregisterEvent;
    api_.register_command = &ModManager::hostRegisterCommand;
}

ModManager::~ModManager() {
    shutdown();
}

bool ModManager::loadDirectory(const std::string& directory) {
    std::lock_guard lock(mutex_);
    if (directory.empty()) {
        return true;
    }
    std::error_code error;
    const std::filesystem::path root(directory);
    if (!std::filesystem::exists(root, error)) {
        Log::warn("mod directory {} not found", directory);
        return true;
    }
    if (!std::filesystem::is_directory(root, error)) {
        Log::error("mod path {} is not a directory", directory);
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto extension = entry.path().extension().string();
        if (extension == ".so" || extension == ".dylib") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    bool success = true;
    for (const auto& path : files) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* message = dlerror();
            Log::error("failed to load mod {}: {}", path.string(), message ? message : "unknown error");
            success = false;
            continue;
        }

        auto init = reinterpret_cast<McModInitFn>(dlsym(handle, "mc_mod_init"));
        if (!init) {
            Log::error("mod {} has no mc_mod_init", path.string());
            dlclose(handle);
            success = false;
            continue;
        }

        const auto nameFn = reinterpret_cast<McModNameFn>(dlsym(handle, "mc_mod_name"));
        const auto versionFn = reinterpret_cast<McModVersionFn>(dlsym(handle, "mc_mod_version"));
        std::string name = nameFn && nameFn() ? nameFn() : path.stem().string();
        std::string version = versionFn && versionFn() ? versionFn() : "1";

        currentHandle_ = handle;
        int result = 0;
        try {
            result = init(&api_);
        } catch (...) {
            Log::error("mod {} threw during initialization", name);
            result = -1;
        }
        currentHandle_ = nullptr;
        if (result != 0) {
            Log::error("mod {} failed to initialize (code {})", name, result);
            removeBindings(handle);
            dlclose(handle);
            success = false;
            continue;
        }

        LoadedMod mod;
        mod.handle = handle;
        mod.path = path.string();
        mod.name = name;
        mod.version = version;
        mod.start = reinterpret_cast<McModStartFn>(dlsym(handle, "mc_mod_start"));
        mod.stop = reinterpret_cast<McModStopFn>(dlsym(handle, "mc_mod_stop"));
        mod.shutdown = reinterpret_cast<McModShutdownFn>(dlsym(handle, "mc_mod_shutdown"));
        mods_.push_back(std::move(mod));
        Log::info("loaded mod {} {} from {}", name, version, path.string());
    }

    if (!mods_.empty()) {
        Log::info("loaded {} mod(s)", mods_.size());
    }
    return success;
}

void ModManager::start() {
    std::lock_guard lock(mutex_);
    for (auto& mod : mods_) {
        if (!mod.start) {
            continue;
        }
        try {
            mod.start();
        } catch (...) {
            Log::error("mod start failed: {}", mod.name);
        }
    }
    active_ = true;
}

void ModManager::stop() {
    std::lock_guard lock(mutex_);
    for (auto& mod : mods_) {
        if (!mod.stop) {
            continue;
        }
        try {
            mod.stop();
        } catch (...) {
            Log::error("mod stop failed: {}", mod.name);
        }
    }
    active_ = false;
}

void ModManager::shutdown() {
    std::lock_guard lock(mutex_);
    for (auto& mod : mods_) {
        if (mod.shutdown) {
            try {
                mod.shutdown();
            } catch (...) {
                Log::error("mod shutdown failed: {}", mod.name);
            }
        }
        if (mod.handle) {
            dlclose(mod.handle);
        }
    }
    mods_.clear();
    events_.clear();
    commands_.clear();
    currentHandle_ = nullptr;
    active_ = false;
}

void ModManager::fire(const std::string& event, const std::vector<ModValue>& arguments) {
    std::vector<EventBinding> callbacks;
    {
        std::lock_guard lock(mutex_);
        for (const auto& binding : events_) {
            if (binding.event == event) {
                callbacks.push_back(binding);
            }
        }
    }
    if (callbacks.empty()) {
        return;
    }

    std::vector<McModValue> values;
    values.reserve(arguments.size());
    for (const auto& argument : arguments) {
        McModValue value{};
        value.type = argument.isString ? MC_MOD_VALUE_STRING : MC_MOD_VALUE_INTEGER;
        value.integer = argument.number;
        value.string = argument.isString ? argument.text.c_str() : nullptr;
        values.push_back(value);
    }

    for (const auto& binding : callbacks) {
        if (!binding.callback) {
            continue;
        }
        try {
            binding.callback(binding.user, event.c_str(), values.data(), values.size());
        } catch (...) {
            Log::error("mod event callback failed: {}", event);
        }
    }
}

bool ModManager::executeCommand(std::string_view sender,
                                std::int32_t entityId,
                                bool isOp,
                                std::string_view name,
                                std::string_view arguments) {
    const std::string key = normalizeName(name);
    CommandBinding binding;
    {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const CommandBinding& candidate) {
            return candidate.name == key;
        });
        if (it == commands_.end()) {
            return false;
        }
        binding = *it;
    }
    if (!binding.callback) {
        return false;
    }
    const std::string senderText(sender);
    const std::string argumentText(arguments);
    McModCommandContext context{};
    context.sender = senderText.c_str();
    context.entity_id = entityId;
    context.is_op = isOp ? 1 : 0;
    context.args = argumentText.c_str();
    try {
        binding.callback(binding.user, &context);
    } catch (...) {
        Log::error("mod command callback failed: {}", key);
    }
    return true;
}

std::vector<std::string> ModManager::list() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    result.reserve(mods_.size());
    for (const auto& mod : mods_) {
        result.push_back(mod.name + " " + mod.version + " (" + mod.path + ")");
    }
    return result;
}

std::size_t ModManager::count() const {
    std::lock_guard lock(mutex_);
    return mods_.size();
}

void ModManager::removeBindings(void* handle) {
    events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const EventBinding& binding) {
        return binding.handle == handle;
    }), events_.end());
    commands_.erase(std::remove_if(commands_.begin(), commands_.end(), [&](const CommandBinding& binding) {
        return binding.handle == handle;
    }), commands_.end());
}

std::string ModManager::normalizeName(std::string_view value) {
    return lowerCopy(value);
}

void ModManager::hostLog(void* host, std::int32_t level, const char* message) {
    if (!host || !message) {
        return;
    }
    if (level == MC_MOD_LOG_ERROR) {
        Log::error("[mod] {}", message);
    } else if (level == MC_MOD_LOG_WARN) {
        Log::warn("[mod] {}", message);
    } else {
        Log::info("[mod] {}", message);
    }
}

void ModManager::hostBroadcast(void* host, const char* message) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || !message) {
        return;
    }
    manager->server_.broadcastSystem(message);
}

std::int64_t ModManager::hostPlayerCount(void* host) {
    auto* manager = static_cast<ModManager*>(host);
    return manager ? static_cast<std::int64_t>(manager->server_.players().count()) : 0;
}

std::int32_t ModManager::hostIsOp(void* host, const char* name) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || !name) {
        return 0;
    }
    return manager->server_.isOp(name) ? 1 : 0;
}

std::int64_t ModManager::hostGetBlock(void* host, std::int32_t x, std::int32_t y, std::int32_t z) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || y < 0 || y > 255) {
        return 0;
    }
    return static_cast<std::int64_t>(manager->server_.world().getBlock(x, y, z));
}

void ModManager::hostSetBlock(void* host, std::int32_t x, std::int32_t y, std::int32_t z, std::uint16_t state) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || y < 0 || y > 255) {
        return;
    }
    manager->server_.setBlockState(x, y, z, state);
}

std::uint64_t ModManager::hostRegisterEvent(void* host,
                                            const char* event,
                                            McModEventCallback callback,
                                            void* user) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || !event || !callback || !manager->currentHandle_) {
        return 0;
    }
    std::lock_guard lock(manager->mutex_);
    EventBinding binding;
    binding.id = manager->nextId_++;
    binding.event = lowerCopy(event);
    binding.callback = callback;
    binding.user = user;
    binding.handle = manager->currentHandle_;
    manager->events_.push_back(binding);
    return binding.id;
}

void ModManager::hostUnregisterEvent(void* host, std::uint64_t id) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager) {
        return;
    }
    std::lock_guard lock(manager->mutex_);
    manager->events_.erase(std::remove_if(manager->events_.begin(), manager->events_.end(),
        [id](const EventBinding& binding) { return binding.id == id; }), manager->events_.end());
}

std::uint64_t ModManager::hostRegisterCommand(void* host,
                                              const char* name,
                                              const char* description,
                                              McModCommandCallback callback,
                                              void* user) {
    auto* manager = static_cast<ModManager*>(host);
    if (!manager || !name || !callback || !manager->currentHandle_) {
        return 0;
    }
    std::lock_guard lock(manager->mutex_);
    const std::string normalized = normalizeName(name);
    if (normalized.empty()) {
        return 0;
    }
    for (const auto& existing : manager->commands_) {
        if (existing.name == normalized) {
            Log::warn("mod command {} is already registered", normalized);
            return 0;
        }
    }
    CommandBinding binding;
    binding.name = normalized;
    binding.description = description ? description : "";
    binding.callback = callback;
    binding.user = user;
    binding.handle = manager->currentHandle_;
    manager->commands_.push_back(std::move(binding));
    return manager->nextId_++;
}

}
