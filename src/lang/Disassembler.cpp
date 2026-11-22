#include "lang/Disassembler.hpp"

#include <string>
#include <unordered_map>

namespace mc::lang {
namespace {

std::string operand(std::uint8_t value) {
    if (value == kImmediate) {
        return "imm";
    }
    if (value == kNoIndex) {
        return "none";
    }
    if (value < 12) {
        return "$r" + std::to_string(value);
    }
    return "$a" + std::to_string(value - 12);
}

std::string memoryOperand(const Instruction& instruction, bool store) {
    std::string text = "[";
    text += operand(store ? instruction.dst : instruction.src);
    if (instruction.index != kNoIndex) {
        text += " + " + operand(instruction.index);
    } else if (instruction.imm != 0) {
        text += " + #" + std::to_string(instruction.imm);
    }
    text += "]";
    return text;
}

std::string target(const std::unordered_map<std::size_t, std::string>& names, Word value) {
    const auto it = names.find(static_cast<std::size_t>(value));
    if (it != names.end()) {
        return "@" + it->second;
    }
    return std::to_string(value);
}

}

std::string disassemble(const Program& program) {
    std::unordered_map<std::size_t, std::string> names;
    for (const auto& [name, entry] : program.functions) {
        names[entry] = name;
    }

    std::string text = "program " + program.origin + "\n";
    for (const auto& [name, entry] : program.functions) {
        text += "oracle @" + name + " = " + std::to_string(entry) + "\n";
    }
    for (std::size_t i = 0; i < program.code.size(); ++i) {
        const Instruction& instruction = program.code[i];
        text += std::to_string(i) + " " + opName(instruction.op) + " " + std::to_string(instruction.line)
            + ":" + std::to_string(instruction.column) + " ";
        switch (instruction.op) {
        case Op::Mov:
            text += operand(instruction.dst) + ", ";
            text += instruction.src == kImmediate
                ? "#" + std::to_string(instruction.imm)
                : operand(instruction.src);
            break;
        case Op::Load:
        case Op::Load8:
            text += operand(instruction.dst) + ", " + memoryOperand(instruction, false);
            break;
        case Op::Store:
        case Op::Store8:
            text += memoryOperand(instruction, true) + ", ";
            text += instruction.src == kImmediate
                ? "#" + std::to_string(instruction.imm2)
                : operand(instruction.src);
            break;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Shl:
        case Op::Shr:
        case Op::Rol:
        case Op::Ror:
        case Op::Cmp:
            text += operand(instruction.dst) + ", ";
            text += instruction.src == kImmediate
                ? "#" + std::to_string(instruction.imm)
                : operand(instruction.src);
            break;
        case Op::Neg:
        case Op::Not:
        case Op::Inc:
        case Op::Dec:
        case Op::Popcount:
        case Op::Clz:
        case Op::Ctz:
        case Op::Bswap:
        case Op::Push:
        case Op::Pop:
            text += operand(instruction.dst);
            break;
        case Op::Swap:
            text += operand(instruction.dst) + ", " + operand(instruction.src);
            break;
        case Op::Jmp:
        case Op::Je:
        case Op::Jne:
        case Op::Jl:
        case Op::Jg:
        case Op::Jle:
        case Op::Jge:
        case Op::Jz:
        case Op::Jnz:
        case Op::Jc:
        case Op::Jnc:
        case Op::Jo:
        case Op::Jno:
        case Op::Call:
            text += target(names, instruction.imm);
            break;
        case Op::Host:
            text += "#" + std::to_string(instruction.imm);
            break;
        case Op::Trap:
            text += "#" + std::to_string(instruction.imm);
            break;
        case Op::Ret:
        case Op::Halt:
        case Op::Nop:
            break;
        }
        text += "\n";
    }
    return text;
}

}
