#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::lang {

using Word = std::int64_t;

enum class Op : std::uint8_t {
    Mov,
    Load,
    Store,
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
    Cmp,
    Jmp,
    Je,
    Jne,
    Jl,
    Jg,
    Jle,
    Jge,
    Call,
    Ret,
    Push,
    Pop,
    Host,
    Halt,
    Nop,
};

inline constexpr std::uint8_t kImmediate = 0xFF;

struct Instruction {
    Op op{Op::Nop};
    std::uint8_t dst{0};
    std::uint8_t src{0};
    Word imm{0};
    Word imm2{0};
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
