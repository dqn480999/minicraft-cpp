#include "common/Uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/Crypto.hpp"
#include "common/Types.hpp"

namespace mc {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}

bool Uuid::isNil() const {
    return std::all_of(bytes.begin(), bytes.end(), [](u8 value) { return value == 0; });
}

std::string Uuid::toString() const {
    const std::string hex = hexString(bytes);
    std::string result;
    result.reserve(36);
    result.append(hex, 0, 8);
    result.push_back('-');
    result.append(hex, 8, 4);
    result.push_back('-');
    result.append(hex, 12, 4);
    result.push_back('-');
    result.append(hex, 16, 4);
    result.push_back('-');
    result.append(hex, 20, 12);
    return result;
}

Uuid Uuid::fromString(std::string_view text) {
    std::array<char, 32> digits{};
    std::size_t count = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        if (count >= digits.size() || hexDigit(c) < 0) {
            throw ProtocolError("invalid uuid");
        }
        digits[count++] = c;
    }
    if (count != digits.size()) {
        throw ProtocolError("invalid uuid length");
    }
    Uuid uuid;
    for (std::size_t i = 0; i < 16; ++i) {
        uuid.bytes[i] = static_cast<u8>((hexDigit(digits[i * 2]) << 4) | hexDigit(digits[i * 2 + 1]));
    }
    return uuid;
}

Uuid Uuid::offline(std::string_view name) {
    const std::string input = "OfflinePlayer:" + std::string(name);
    const auto hash = md5(std::span<const u8>(reinterpret_cast<const u8*>(input.data()), input.size()));
    Uuid uuid;
    std::copy(hash.begin(), hash.end(), uuid.bytes.begin());
    uuid.bytes[6] = static_cast<u8>((uuid.bytes[6] & 0x0F) | 0x30);
    uuid.bytes[8] = static_cast<u8>((uuid.bytes[8] & 0x3F) | 0x80);
    return uuid;
}

Uuid Uuid::random() {
    const auto data = randomBytes(16);
    Uuid uuid;
    std::copy(data.begin(), data.end(), uuid.bytes.begin());
    uuid.bytes[6] = static_cast<u8>((uuid.bytes[6] & 0x0F) | 0x40);
    uuid.bytes[8] = static_cast<u8>((uuid.bytes[8] & 0x3F) | 0x80);
    return uuid;
}

bool Uuid::operator==(const Uuid& other) const {
    return bytes == other.bytes;
}

bool Uuid::operator!=(const Uuid& other) const {
    return !(*this == other);
}

}
