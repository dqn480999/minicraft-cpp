#pragma once

#include <string>

#include "lang/Bytecode.hpp"

namespace mc::lang {

std::string disassemble(const Program& program);

}
