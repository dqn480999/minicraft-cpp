#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "lang/Bytecode.hpp"

namespace mc::lang {

class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void broadcastInteger(Word value) = 0;
    virtual void broadcastText(const std::string& text) = 0;
    virtual Word playerCount() = 0;
    virtual Word currentTimeMs() = 0;
    virtual Word getBlock(Word x, Word y, Word z) = 0;
    virtual void setBlock(Word x, Word y, Word z, Word state) = 0;
    virtual void logInteger(Word value) = 0;
};

class VirtualMachine {
public:
    VirtualMachine(const Program& program, ScriptHost& host);

    bool run(const std::string& function,
             const std::vector<Word>& arguments,
             const std::vector<std::string>& runtimeStrings,
             std::string& error);

private:
    bool resolveString(Word id, std::string& out) const;
    bool address(Word value, std::size_t& out) const;

    const Program& program_;
    ScriptHost& host_;
    std::vector<Word> registers_;
    std::vector<Word> memory_;
    std::vector<Word> stack_;
    std::unordered_map<Word, std::string> runtimeStrings_;
    Word flags_{0};
};

}
