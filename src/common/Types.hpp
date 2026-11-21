#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

using u8 = std::uint8_t;
using i8 = std::int8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

using Bytes = std::vector<u8>;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};

class BufferUnderflow : public ProtocolError {
public:
    explicit BufferUnderflow(const std::string& message) : ProtocolError(message) {}
};

inline constexpr std::string_view kServerVersion = "1.8.9";
inline constexpr i32 kProtocolVersion = 47;
inline constexpr std::size_t kMaxStringLength = 32767;

}
