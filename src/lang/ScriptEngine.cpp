#include "lang/ScriptEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "common/Log.hpp"
#include "lang/Compiler.hpp"
#include "server/Server.hpp"
#include "world/Blocks.hpp"

namespace mc {

ScriptEngine::ScriptEngine(Server& server)
    : server_(server) {
}

bool ScriptEngine::loadDirectory(const std::string& directory) {
    std::lock_guard lock(mutex_);
    if (directory.empty()) {
        return true;
    }
    std::error_code error;
    const std::filesystem::path root(directory);
    if (!std::filesystem::exists(root, error)) {
        Log::warn("script directory {} not found", directory);
        return true;
    }
    if (!std::filesystem::is_directory(root, error)) {
        Log::warn("script path {} is not a directory", directory);
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
        if (entry.path().extension() == ".obl") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    bool success = true;
    std::size_t loaded = 0;
    for (const auto& path : files) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            Log::error("failed to read script {}", path.string());
            success = false;
            continue;
        }
        std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        lang::Program program;
        std::string compileError;
        if (!lang::Compiler::compileInto(program, source, path.string(), compileError)) {
            Log::error("script error: {}", compileError);
            success = false;
            continue;
        }
        bool resolved = true;
        for (const auto& patch : program.callPatches) {
            const auto target = program.functions.find(patch.name);
            if (target == program.functions.end()) {
                Log::error("script {}: unknown oracle {}", path.string(), patch.name);
                resolved = false;
                break;
            }
            program.code[patch.instruction].imm = static_cast<lang::Word>(target->second);
        }
        program.callPatches.clear();
        if (!resolved) {
            success = false;
            continue;
        }
        functionCount_ += program.functions.size();
        programs_.push_back(std::move(program));
        ++loaded;
    }

    if (loaded > 0) {
        Log::info("loaded {} script(s), {} oracle(s)", loaded, functionCount_);
    }
    return success;
}

bool ScriptEngine::reload(const std::string& directory) {
    std::lock_guard lock(mutex_);
    programs_.clear();
    functionCount_ = 0;
    return loadDirectory(directory);
}

void ScriptEngine::fire(const std::string& event,
                        const std::vector<lang::Word>& arguments,
                        const std::vector<std::string>& runtimeStrings) {
    std::lock_guard lock(mutex_);
    for (const auto& program : programs_) {
        if (!program.functions.contains(event)) {
            continue;
        }
        std::vector<lang::Word> callArguments;
        callArguments.reserve(runtimeStrings.size() + arguments.size());
        for (std::size_t i = 0; i < runtimeStrings.size(); ++i) {
            callArguments.push_back(100000 + static_cast<lang::Word>(i));
        }
        callArguments.insert(callArguments.end(), arguments.begin(), arguments.end());
        lang::VirtualMachine vm(program, *this);
        std::string error;
        if (!vm.run(event, callArguments, runtimeStrings, error)) {
            Log::error("script error: {}", error);
        }
    }
}

std::size_t ScriptEngine::functionCount() const {
    return functionCount_;
}

void ScriptEngine::broadcastInteger(lang::Word value) {
    server_.broadcastSystem(std::to_string(value));
}

void ScriptEngine::broadcastText(const std::string& text) {
    server_.broadcastSystem(text);
}

lang::Word ScriptEngine::playerCount() {
    return static_cast<lang::Word>(server_.players().count());
}

lang::Word ScriptEngine::currentTimeMs() {
    return static_cast<lang::Word>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

lang::Word ScriptEngine::getBlock(lang::Word x, lang::Word y, lang::Word z) {
    if (y < 0 || y > 255) {
        return 0;
    }
    const auto block = server_.world().getBlock(
        static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z));
    return static_cast<lang::Word>(block);
}

void ScriptEngine::setBlock(lang::Word x, lang::Word y, lang::Word z, lang::Word state) {
    if (y < 0 || y > 255) {
        return;
    }
    server_.setBlockState(
        static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z),
        static_cast<u16>(state & 0xFFFF));
}

void ScriptEngine::logInteger(lang::Word value) {
    Log::info("[script] {}", value);
}

}
