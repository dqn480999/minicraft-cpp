#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "lang/Bytecode.hpp"
#include "lang/Vm.hpp"

namespace mc {

class Server;

class ScriptEngine final : private lang::ScriptHost {
public:
    explicit ScriptEngine(Server& server);
    ~ScriptEngine() override = default;

    bool loadDirectory(const std::string& directory);
    bool reload(const std::string& directory);
    void fire(const std::string& event,
              const std::vector<lang::Word>& arguments = {},
              const std::vector<std::string>& runtimeStrings = {});
    std::size_t functionCount() const;

private:
    void broadcastInteger(lang::Word value) override;
    void broadcastText(const std::string& text) override;
    lang::Word playerCount() override;
    lang::Word currentTimeMs() override;
    lang::Word getBlock(lang::Word x, lang::Word y, lang::Word z) override;
    void setBlock(lang::Word x, lang::Word y, lang::Word z, lang::Word state) override;
    void logInteger(lang::Word value) override;

    Server& server_;
    mutable std::recursive_mutex mutex_;
    std::vector<lang::Program> programs_;
    std::size_t functionCount_{0};
};

}
