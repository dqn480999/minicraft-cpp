#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "common/Types.hpp"

namespace mc {

struct Uuid {
    std::array<u8, 16> bytes{};

    bool isNil() const;
    std::string toString() const;

    static Uuid fromString(std::string_view text);
    static Uuid offline(std::string_view name);
    static Uuid random();

    bool operator==(const Uuid& other) const;
    bool operator!=(const Uuid& other) const;
};

}
