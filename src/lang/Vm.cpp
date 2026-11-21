#include "lang/Vm.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>

namespace mc::lang {
namespace {

constexpr std::size_t kMemoryCells = 4096;
constexpr std::size_t kStackLimit = 256;
constexpr std::size_t kInstructionLimit = 200000;
constexpr Word kRuntimeStringBase = 100000;

Word wrappingAdd(Word lhs, Word rhs) {
    return static_cast<Word>(static_cast<std::uint64_t>(lhs) + static_cast<std::uint64_t>(rhs));
}

Word wrappingSub(Word lhs, Word rhs) {
    return static_cast<Word>(static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs));
}

Word wrappingMul(Word lhs, Word rhs) {
    return static_cast<Word>(static_cast<std::uint64_t>(lhs) * static_cast<std::uint64_t>(rhs));
}

}

VirtualMachine::VirtualMachine(const Program& program, ScriptHost& host)
    : program_(program)
    , host_(host)
    , registers_(16, 0)
    , memory_(kMemoryCells, 0) {
}

bool VirtualMachine::resolveString(Word id, std::string& out) const {
    if (id >= kRuntimeStringBase) {
        const auto it = runtimeStrings_.find(id);
        if (it == runtimeStrings_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    if (id < 0 || static_cast<std::size_t>(id) >= program_.strings.size()) {
        return false;
    }
    out = program_.strings[static_cast<std::size_t>(id)];
    return true;
}

bool VirtualMachine::address(Word value, std::size_t& out) const {
    if (value < 0 || static_cast<std::uint64_t>(value) >= memory_.size()) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool VirtualMachine::run(const std::string& function,
                         const std::vector<Word>& arguments,
                         const std::vector<std::string>& runtimeStrings,
                         std::string& error) {
    const auto it = program_.functions.find(function);
    if (it == program_.functions.end()) {
        error = program_.origin + ": unknown oracle " + function;
        return false;
    }
    if (arguments.size() > 4) {
        error = program_.origin + ": too many arguments for " + function;
        return false;
    }

    registers_.assign(16, 0);
    memory_.assign(kMemoryCells, 0);
    stack_.clear();
    runtimeStrings_.clear();
    flags_ = 0;

    for (std::size_t i = 0; i < runtimeStrings.size(); ++i) {
        runtimeStrings_[kRuntimeStringBase + static_cast<Word>(i)] = runtimeStrings[i];
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        registers_[12 + i] = arguments[i];
    }

    const auto fail = [&](const std::string& message, std::size_t pc) {
        error = program_.origin + ": " + function + " pc " + std::to_string(pc) + ": " + message;
        return false;
    };

    std::size_t pc = it->second;
    std::size_t steps = 0;
    while (true) {
        if (pc >= program_.code.size()) {
            return fail("execution left code", pc);
        }
        if (++steps > kInstructionLimit) {
            return fail("instruction limit reached", pc);
        }
        const Instruction instruction = program_.code[pc];
        ++pc;

        switch (instruction.op) {
        case Op::Mov:
            registers_[instruction.dst] = instruction.src == kImmediate
                ? instruction.imm
                : registers_[instruction.src];
            break;
        case Op::Load: {
            const Word pointer = registers_[instruction.src] + instruction.imm;
            std::size_t index = 0;
            if (!address(pointer, index)) {
                return fail("memory read out of range", pc);
            }
            registers_[instruction.dst] = memory_[index];
            break;
        }
        case Op::Store: {
            const Word pointer = registers_[instruction.dst] + instruction.imm;
            std::size_t index = 0;
            if (!address(pointer, index)) {
                return fail("memory write out of range", pc);
            }
            memory_[index] = instruction.src == kImmediate
                ? instruction.imm2
                : registers_[instruction.src];
            break;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Shl:
        case Op::Shr: {
            const Word lhs = registers_[instruction.dst];
            const Word rhs = instruction.src == kImmediate ? instruction.imm : registers_[instruction.src];
            Word result = 0;
            switch (instruction.op) {
            case Op::Add:
                result = wrappingAdd(lhs, rhs);
                break;
            case Op::Sub:
                result = wrappingSub(lhs, rhs);
                break;
            case Op::Mul:
                result = wrappingMul(lhs, rhs);
                break;
            case Op::Div:
                if (rhs == 0) {
                    return fail("division by zero", pc);
                }
                if (lhs == std::numeric_limits<Word>::min() && rhs == -1) {
                    result = lhs;
                } else {
                    result = lhs / rhs;
                }
                break;
            case Op::Mod:
                if (rhs == 0) {
                    return fail("modulo by zero", pc);
                }
                result = lhs == std::numeric_limits<Word>::min() && rhs == -1 ? 0 : lhs % rhs;
                break;
            case Op::And:
                result = lhs & rhs;
                break;
            case Op::Or:
                result = lhs | rhs;
                break;
            case Op::Xor:
                result = lhs ^ rhs;
                break;
            case Op::Shl:
                result = static_cast<Word>(static_cast<std::uint64_t>(lhs)
                    << (static_cast<std::uint64_t>(rhs) & 63U));
                break;
            case Op::Shr:
                result = lhs >> (static_cast<std::uint64_t>(rhs) & 63U);
                break;
            default:
                break;
            }
            registers_[instruction.dst] = result;
            break;
        }
        case Op::Cmp: {
            const Word lhs = registers_[instruction.dst];
            const Word rhs = instruction.src == kImmediate ? instruction.imm : registers_[instruction.src];
            flags_ = lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
            break;
        }
        case Op::Jmp:
            pc = static_cast<std::size_t>(instruction.imm);
            break;
        case Op::Je:
            if (flags_ == 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jne:
            if (flags_ != 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jl:
            if (flags_ < 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jg:
            if (flags_ > 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jle:
            if (flags_ <= 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jge:
            if (flags_ >= 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Call:
            if (stack_.size() >= kStackLimit) {
                return fail("call stack overflow", pc);
            }
            stack_.push_back(static_cast<Word>(pc));
            pc = static_cast<std::size_t>(instruction.imm);
            break;
        case Op::Ret:
            if (stack_.empty()) {
                return true;
            }
            pc = static_cast<std::size_t>(stack_.back());
            stack_.pop_back();
            break;
        case Op::Push:
            if (stack_.size() >= kStackLimit) {
                return fail("stack overflow", pc);
            }
            stack_.push_back(registers_[instruction.dst]);
            break;
        case Op::Pop:
            if (stack_.empty()) {
                return fail("stack underflow", pc);
            }
            registers_[instruction.dst] = stack_.back();
            stack_.pop_back();
            break;
        case Op::Host: {
            const Word id = instruction.imm;
            const Word a0 = registers_[12];
            const Word a1 = registers_[13];
            const Word a2 = registers_[14];
            const Word a3 = registers_[15];
            switch (id) {
            case 1:
                host_.broadcastInteger(a0);
                break;
            case 2: {
                std::string text;
                if (!resolveString(a0, text)) {
                    return fail("unknown string id", pc);
                }
                host_.broadcastText(text);
                break;
            }
            case 3:
                registers_[0] = host_.playerCount();
                break;
            case 4:
                registers_[0] = host_.currentTimeMs();
                break;
            case 5:
                registers_[0] = host_.getBlock(a0, a1, a2);
                break;
            case 6:
                host_.setBlock(a0, a1, a2, a3);
                break;
            case 7:
                host_.logInteger(a0);
                break;
            default:
                return fail("unknown host call " + std::to_string(id), pc);
            }
            break;
        }
        case Op::Halt:
            return true;
        case Op::Nop:
            break;
        }
    }
}

}
