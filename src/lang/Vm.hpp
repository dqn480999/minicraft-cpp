#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lang/Bytecode.hpp"

namespace mc::lang {

class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void broadcastInteger(Word value) = 0;
    virtual void broadcastText(const std::string& text) = 0;
    virtual void broadcastLabeledInteger(std::string_view label, Word value) = 0;
    virtual Word playerCount() = 0;
    virtual Word currentTimeMs() = 0;
    virtual Word getBlock(Word x, Word y, Word z) = 0;
    virtual void setBlock(Word x, Word y, Word z, Word state) = 0;
    virtual void logInteger(Word value) = 0;
    virtual void traceLine(std::string_view text) = 0;
};

class VirtualMachine {
public:
    VirtualMachine(const Program& program, ScriptHost& host, std::vector<Word>& memory);

    void setTrace(bool value);
    bool run(const std::string& function,
             const std::vector<Word>& arguments,
             const std::vector<std::string>& runtimeStrings,
             std::string& error,
             Word instructionLimit = 200000);

private:
    bool resolveString(Word id, std::string& out) const;
    bool address(Word base, std::uint8_t index, Word offset, std::size_t& out) const;
    bool pushData(Word value);
    bool popData(Word& value);
    void setZeroNegative(Word result);
    void setAddFlags(Word lhs, Word rhs, Word result);
    void setSubFlags(Word lhs, Word rhs, Word result);

    const Program& program_;
    ScriptHost& host_;
    std::vector<Word>& memory_;
    std::vector<Word> registers_;
    std::vector<Word> dataStack_;
    std::vector<std::size_t> callStack_;
    std::unordered_map<Word, std::string> runtimeStrings_;
    Word compare_{0};
    bool zero_{true};
    bool negative_{false};
    bool carry_{false};
    bool overflow_{false};
    bool trace_{false};
};

}
