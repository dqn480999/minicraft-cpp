#pragma once

#include <cstddef>
#include <span>

#include "common/Types.hpp"

namespace mc {

Bytes zlibCompress(std::span<const u8> data);
Bytes zlibDecompress(std::span<const u8> data, std::size_t expectedSize, std::size_t maxSize);

}
