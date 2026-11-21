#include "lang/Vm.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace mc::lang {
namespace {

constexpr std::size_t kDataStackLimit = 256;
constexpr std::size_t kCallStackLimit = 64;
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

bool addOverflow(Word lhs, Word rhs, Word result) {
    return ((lhs ^ result) & (rhs ^ result)) < 0;
}

bool subOverflow(Word lhs, Word rhs, Word result) {
    return ((lhs ^ rhs) & (lhs ^ result)) < 0;
}

}

VirtualMachine::VirtualMachine(const Program& program, ScriptHost& host, std::vector<Word>& memory)
    : program_(program)
    , host_(host)
    , memory_(memory)
    , registers_(16, 0) {
}

void VirtualMachine::setTrace(bool value) {
    trace_ = value;
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

bool VirtualMachine::address(Word base, std::uint8_t index, Word offset, std::size_t& out) const {
    Word value = wrappingAdd(base, offset);
    if (index != kNoIndex) {
        value = wrappingAdd(value, registers_[index]);
    }
    if (value < 0 || static_cast<std::uint64_t>(value) >= memory_.size()) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool VirtualMachine::pushData(Word value) {
    if (dataStack_.size() >= kDataStackLimit) {
        return false;
    }
    dataStack_.push_back(value);
    return true;
}

bool VirtualMachine::popData(Word& value) {
    if (dataStack_.empty()) {
        return false;
    }
    value = dataStack_.back();
    dataStack_.pop_back();
    return true;
}

void VirtualMachine::setZeroNegative(Word result) {
    zero_ = result == 0;
    negative_ = result < 0;
}

void VirtualMachine::setAddFlags(Word lhs, Word rhs, Word result) {
    setZeroNegative(result);
    carry_ = static_cast<std::uint64_t>(lhs) + static_cast<std::uint64_t>(rhs)
        < static_cast<std::uint64_t>(lhs);
    overflow_ = addOverflow(lhs, rhs, result);
}

void VirtualMachine::setSubFlags(Word lhs, Word rhs, Word result) {
    setZeroNegative(result);
    carry_ = static_cast<std::uint64_t>(lhs) < static_cast<std::uint64_t>(rhs);
    overflow_ = subOverflow(lhs, rhs, result);
}

bool VirtualMachine::run(const std::string& function,
                         const std::vector<Word>& arguments,
                         const std::vector<std::string>& runtimeStrings,
                         std::string& error,
                         Word instructionLimit) {
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
    dataStack_.clear();
    callStack_.clear();
    runtimeStrings_.clear();
    compare_ = 0;
    zero_ = true;
    negative_ = false;
    carry_ = false;
    overflow_ = false;

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

    const Word limit = instructionLimit > 0 ? instructionLimit : 200000;
    std::size_t pc = it->second;
    Word steps = 0;
    while (true) {
        if (pc >= program_.code.size()) {
            return fail("execution left code", pc);
        }
        if (++steps > limit) {
            return fail("instruction limit reached", pc);
        }
        const Instruction& instruction = program_.code[pc];

        if (trace_) {
            std::string line = function + " pc " + std::to_string(pc) + " "
                + opName(instruction.op) + " " + std::to_string(instruction.line) + ":"
                + std::to_string(instruction.column);
            host_.traceLine(line);
        }

        ++pc;

        switch (instruction.op) {
        case Op::Mov:
            registers_[instruction.dst] = instruction.src == kImmediate
                ? instruction.imm
                : registers_[instruction.src];
            break;
        case Op::Load:
        case Op::Load8: {
            std::size_t index = 0;
            if (!address(registers_[instruction.src], instruction.index, instruction.imm, index)) {
                return fail("memory read out of range", pc);
            }
            const Word value = memory_[index];
            registers_[instruction.dst] = instruction.op == Op::Load8 ? (value & 0xFF) : value;
            break;
        }
        case Op::Store:
        case Op::Store8: {
            std::size_t index = 0;
            if (!address(registers_[instruction.dst], instruction.index, instruction.imm, index)) {
                return fail("memory write out of range", pc);
            }
            const Word value = instruction.src == kImmediate
                ? instruction.imm2
                : registers_[instruction.src];
            memory_[index] = instruction.op == Op::Store8 ? (value & 0xFF) : value;
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
                setAddFlags(lhs, rhs, result);
                break;
            case Op::Sub:
                result = wrappingSub(lhs, rhs);
                setSubFlags(lhs, rhs, result);
                break;
            case Op::Mul:
                result = wrappingMul(lhs, rhs);
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Div:
                if (rhs == 0) {
                    return fail("division by zero", pc);
                }
                result = lhs == std::numeric_limits<Word>::min() && rhs == -1 ? lhs : lhs / rhs;
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Mod:
                if (rhs == 0) {
                    return fail("modulo by zero", pc);
                }
                result = lhs == std::numeric_limits<Word>::min() && rhs == -1 ? 0 : lhs % rhs;
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::And:
                result = lhs & rhs;
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Or:
                result = lhs | rhs;
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Xor:
                result = lhs ^ rhs;
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Shl:
                result = static_cast<Word>(static_cast<std::uint64_t>(lhs)
                    << (static_cast<std::uint64_t>(rhs) & 63U));
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            case Op::Shr:
                result = lhs >> (static_cast<std::uint64_t>(rhs) & 63U);
                setZeroNegative(result);
                carry_ = false;
                overflow_ = false;
                break;
            default:
                break;
            }
            registers_[instruction.dst] = result;
            break;
        }
        case Op::Neg: {
            const Word value = registers_[instruction.dst];
            const Word result = wrappingSub(0, value);
            registers_[instruction.dst] = result;
            setSubFlags(0, value, result);
            break;
        }
        case Op::Not: {
            const Word result = ~registers_[instruction.dst];
            registers_[instruction.dst] = result;
            setZeroNegative(result);
            carry_ = false;
            overflow_ = false;
            break;
        }
        case Op::Inc: {
            const Word value = registers_[instruction.dst];
            const Word result = wrappingAdd(value, 1);
            registers_[instruction.dst] = result;
            setAddFlags(value, 1, result);
            break;
        }
        case Op::Dec: {
            const Word value = registers_[instruction.dst];
            const Word result = wrappingSub(value, 1);
            registers_[instruction.dst] = result;
            setSubFlags(value, 1, result);
            break;
        }
        case Op::Swap:
            std::swap(registers_[instruction.dst], registers_[instruction.src]);
            break;
        case Op::Cmp: {
            const Word lhs = registers_[instruction.dst];
            const Word rhs = instruction.src == kImmediate ? instruction.imm : registers_[instruction.src];
            const Word result = wrappingSub(lhs, rhs);
            compare_ = result < 0 ? -1 : (result > 0 ? 1 : 0);
            setSubFlags(lhs, rhs, result);
            break;
        }
        case Op::Jmp:
            pc = static_cast<std::size_t>(instruction.imm);
            break;
        case Op::Je:
            if (compare_ == 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jne:
            if (compare_ != 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jl:
            if (compare_ < 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jg:
            if (compare_ > 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jle:
            if (compare_ <= 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jge:
            if (compare_ >= 0) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jz:
            if (zero_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jnz:
            if (!zero_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jc:
            if (carry_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jnc:
            if (!carry_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jo:
            if (overflow_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Jno:
            if (!overflow_) {
                pc = static_cast<std::size_t>(instruction.imm);
            }
            break;
        case Op::Call:
            if (callStack_.size() >= kCallStackLimit) {
                return fail("call stack overflow", pc);
            }
            callStack_.push_back(pc);
            pc = static_cast<std::size_t>(instruction.imm);
            break;
        case Op::Ret:
            if (callStack_.empty()) {
                return true;
            }
            pc = callStack_.back();
            callStack_.pop_back();
            break;
        case Op::Push:
            if (!pushData(registers_[instruction.dst])) {
                return fail("stack overflow", pc);
            }
            break;
        case Op::Pop: {
            Word value = 0;
            if (!popData(value)) {
                return fail("stack underflow", pc);
            }
            registers_[instruction.dst] = value;
            break;
        }
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
        case Op::Trap:
            return fail("trap " + std::to_string(instruction.imm), pc);
        case Op::Halt:
            return true;
        case Op::Nop:
            break;
        }
    }
}

}
