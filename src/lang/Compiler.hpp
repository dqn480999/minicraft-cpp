#pragma once

#include <string>
#include <string_view>

#include "lang/Bytecode.hpp"

namespace mc::lang {

class Compiler {
public:
    static bool compileInto(Program& program, std::string_view source, std::string_view origin, std::string& error);
};

}
