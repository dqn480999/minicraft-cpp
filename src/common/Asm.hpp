#pragma once

#include <cstddef>

#include "common/Types.hpp"

namespace mc {

bool assemblyAvailable();
void fastCopy(void* destination, const void* source, std::size_t size);
void fastZero(void* destination, std::size_t size);
int varIntDecode(const u8* data, std::size_t size, i32& value);
int varIntEncode(i32 value, u8* output);

}
