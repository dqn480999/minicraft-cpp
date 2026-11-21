#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::lang {

using Word = std::int64_t;

inline constexpr std::size_t kMemoryCellCount = 4096;
inline constexpr std::uint8_t kImmediate = 0xFF;
inline constexpr std::uint8_t kNoIndex = 0xFE;

enum class Op : std::uint8_t {
    Mov,
    Load,
    Load8,
    Store,
    Store8,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Neg,
    Not,
    Inc,
    Dec,
    Swap,
    Cmp,
    Jmp,
    Je,
    Jne,
    Jl,
    Jg,
    Jle,
    Jge,
    Jz,
    Jnz,
    Jc,
    Jnc,
    Jo,
    Jno,
    Call,
    Ret,
    Push,
    Pop,
    Host,
    Trap,
    Halt,
    Nop,
};

inline constexpr const char* opName(Op op) {
    switch (op) {
    case Op::Mov: return "MOV";
    case Op::Load: return "LOAD";
    case Op::Load8: return "LOAD8";
    case Op::Store: return "STORE";
    case Op::Store8: return "STORE8";
    case Op::Add: return "ADD";
    case Op::Sub: return "SUB";
    case Op::Mul: return "MUL";
    case Op::Div: return "DIV";
    case Op::Mod: return "MOD";
    case Op::And: return "AND";
    case Op::Or: return "OR";
    case Op::Xor: return "XOR";
    case Op::Shl: return "SHL";
    case Op::Shr: return "SHR";
    case Op::Neg: return "NEG";
    case Op::Not: return "NOT";
    case Op::Inc: return "INC";
    case Op::Dec: return "DEC";
    case Op::Swap: return "SWAP";
    case Op::Cmp: return "CMP";
    case Op::Jmp: return "JMP";
    case Op::Je: return "JE";
    case Op::Jne: return "JNE";
    case Op::Jl: return "JL";
    case Op::Jg: return "JG";
    case Op::Jle: return "JLE";
    case Op::Jge: return "JGE";
    case Op::Jz: return "JZ";
    case Op::Jnz: return "JNZ";
    case Op::Jc: return "JC";
    case Op::Jnc: return "JNC";
    case Op::Jo: return "JO";
    case Op::Jno: return "JNO";
    case Op::Call: return "CALL";
    case Op::Ret: return "RET";
    case Op::Push: return "PUSH";
    case Op::Pop: return "POP";
    case Op::Host: return "HOST";
    case Op::Trap: return "TRAP";
    case Op::Halt: return "HALT";
    case Op::Nop: return "NOP";
    }
    return "?";
}

struct Instruction {
    Op op{Op::Nop};
    std::uint8_t dst{0};
    std::uint8_t src{0};
    std::uint8_t index{kNoIndex};
    Word imm{0};
    Word imm2{0};
    std::uint32_t line{0};
    std::uint32_t column{0};
};

struct CallPatch {
    std::size_t instruction{0};
    std::string name;
};

struct Program {
    std::vector<Instruction> code;
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::size_t> functions;
    std::vector<CallPatch> callPatches;
    std::string origin;
};

}
