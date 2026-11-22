#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mod/ModApi.h"

namespace mc::mod {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

struct Value {
    bool isString{false};
    std::int64_t integer{0};
    std::string text;

    std::int64_t asInt(std::int64_t fallback = 0) const {
        return isString ? fallback : integer;
    }

    const std::string& asString() const {
        return text;
    }
};

class Event {
public:
    Event(std::string name, std::vector<Value> values)
        : name_(std::move(name))
        , values_(std::move(values)) {
    }

    const std::string& name() const {
        return name_;
    }

    std::size_t size() const {
        return values_.size();
    }

    bool isString(std::size_t index) const {
        return index < values_.size() && values_[index].isString;
    }

    std::int64_t asInt(std::size_t index) const {
        return index < values_.size() ? values_[index].asInt() : 0;
    }

    const std::string& asString(std::size_t index) const {
        static const std::string empty;
        return index < values_.size() && values_[index].isString ? values_[index].text : empty;
    }

private:
    std::string name_;
    std::vector<Value> values_;
};

struct CommandContext {
    std::string sender;
    std::int32_t entityId{0};
    bool isOp{false};
    std::string args;
};

class Mod {
public:
    virtual ~Mod() = default;

    int init(const McModApi* api) {
        if (!api || api->abi_version != MC_MOD_ABI_VERSION || !api->host) {
            return 1;
        }
        api_ = api;
        return onInit();
    }

    void start() {
        if (api_) {
            onStart();
        }
    }

    void stop() {
        if (api_) {
            onStop();
        }
    }

    void shutdown() {
        if (api_) {
            onShutdown();
            api_ = nullptr;
        }
        eventCallbacks_.clear();
        commandCallbacks_.clear();
    }

protected:
    virtual int onInit() {
        return 0;
    }

    virtual void onStart() {
    }

    virtual void onStop() {
    }

    virtual void onShutdown() {
    }

    void log(LogLevel level, std::string_view message) {
        if (!api_ || !api_->log) {
            return;
        }
        std::string copy(message);
        api_->log(api_->host, static_cast<std::int32_t>(level), copy.c_str());
    }

    void info(std::string_view message) {
        log(LogLevel::Info, message);
    }

    void warn(std::string_view message) {
        log(LogLevel::Warn, message);
    }

    void error(std::string_view message) {
        log(LogLevel::Error, message);
    }

    void broadcast(std::string_view message) {
        if (!api_ || !api_->broadcast) {
            return;
        }
        std::string copy(message);
        api_->broadcast(api_->host, copy.c_str());
    }

    std::int64_t playerCount() {
        return api_ && api_->player_count ? api_->player_count(api_->host) : 0;
    }

    bool isOp(std::string_view name) {
        if (!api_ || !api_->is_op) {
            return false;
        }
        std::string copy(name);
        return api_->is_op(api_->host, copy.c_str()) != 0;
    }

    std::int64_t getBlock(std::int32_t x, std::int32_t y, std::int32_t z) {
        return api_ && api_->get_block ? api_->get_block(api_->host, x, y, z) : 0;
    }

    void setBlock(std::int32_t x, std::int32_t y, std::int32_t z, std::uint16_t state) {
        if (api_ && api_->set_block) {
            api_->set_block(api_->host, x, y, z, state);
        }
    }

    std::uint64_t on(std::string event, std::function<void(const Event&)> callback) {
        if (!api_ || !api_->register_event || !callback) {
            return 0;
        }
        auto holder = std::make_unique<EventCallback>();
        holder->callback = std::move(callback);
        const std::uint64_t id = api_->register_event(
            api_->host, event.c_str(), &Mod::eventTrampoline, holder.get());
        if (id == 0) {
            return 0;
        }
        eventCallbacks_.push_back(std::move(holder));
        return id;
    }

    bool command(std::string name,
                 std::string description,
                 std::function<void(const CommandContext&)> callback) {
        if (!api_ || !api_->register_command || !callback) {
            return false;
        }
        auto holder = std::make_unique<CommandCallback>();
        holder->callback = std::move(callback);
        const std::uint64_t id = api_->register_command(
            api_->host, name.c_str(), description.c_str(), &Mod::commandTrampoline, holder.get());
        if (id == 0) {
            return false;
        }
        commandCallbacks_.push_back(std::move(holder));
        return true;
    }

private:
    struct EventCallback {
        std::function<void(const Event&)> callback;
    };

    struct CommandCallback {
        std::function<void(const CommandContext&)> callback;
    };

    static void eventTrampoline(void* user, const char* event, const McModValue* args, std::size_t argc) {
        auto* holder = static_cast<EventCallback*>(user);
        if (!holder || !holder->callback) {
            return;
        }
        std::vector<Value> values;
        values.reserve(argc);
        for (std::size_t i = 0; i < argc; ++i) {
            Value value;
            value.isString = args[i].type == MC_MOD_VALUE_STRING;
            value.integer = args[i].integer;
            if (value.isString && args[i].string) {
                value.text = args[i].string;
            }
            values.push_back(std::move(value));
        }
        Event built(event ? event : "", std::move(values));
        holder->callback(built);
    }

    static void commandTrampoline(void* user, const McModCommandContext* context) {
        auto* holder = static_cast<CommandCallback*>(user);
        if (!holder || !holder->callback) {
            return;
        }
        CommandContext commandContext;
        if (context) {
            commandContext.sender = context->sender ? context->sender : "";
            commandContext.entityId = context->entity_id;
            commandContext.isOp = context->is_op != 0;
            commandContext.args = context->args ? context->args : "";
        }
        holder->callback(commandContext);
    }

    const McModApi* api_{nullptr};
    std::vector<std::unique_ptr<EventCallback>> eventCallbacks_;
    std::vector<std::unique_ptr<CommandCallback>> commandCallbacks_;
};

}

#define MC_MOD(TYPE)                                                                              \
    static TYPE mc_mod_instance;                                                                   \
    extern "C" MC_MOD_EXPORT int mc_mod_init(const McModApi* api) {                                \
        return mc_mod_instance.init(api);                                                          \
    }                                                                                              \
    extern "C" MC_MOD_EXPORT void mc_mod_start() {                                                 \
        mc_mod_instance.start();                                                                   \
    }                                                                                              \
    extern "C" MC_MOD_EXPORT void mc_mod_stop() {                                                  \
        mc_mod_instance.stop();                                                                    \
    }                                                                                              \
    extern "C" MC_MOD_EXPORT void mc_mod_shutdown() {                                              \
        mc_mod_instance.shutdown();                                                                \
    }
