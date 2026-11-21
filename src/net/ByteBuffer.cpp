#include "net/ByteBuffer.hpp"

#include <bit>
#include <cstring>

#include "common/Asm.hpp"
#include <limits>

namespace mc {

ByteBuffer::ByteBuffer(Bytes data) : data_(std::move(data)) {}

void ByteBuffer::reserve(std::size_t capacity) {
    data_.reserve(capacity);
}

void ByteBuffer::append(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    data_.insert(data_.end(), bytes, bytes + size);
}

void ByteBuffer::append(std::span<const u8> data) {
    data_.insert(data_.end(), data.begin(), data.end());
}

void ByteBuffer::append(const Bytes& data) {
    data_.insert(data_.end(), data.begin(), data.end());
}

void ByteBuffer::append(const ByteBuffer& other) {
    data_.insert(data_.end(), other.data_.begin(), other.data_.end());
}

std::size_t ByteBuffer::size() const {
    return data_.size();
}

std::size_t ByteBuffer::remaining() const {
    return data_.size() - readPosition_;
}

std::size_t ByteBuffer::readPosition() const {
    return readPosition_;
}

void ByteBuffer::setReadPosition(std::size_t position) {
    if (position > data_.size()) {
        throw BufferUnderflow("invalid read position");
    }
    readPosition_ = position;
}

const Bytes& ByteBuffer::data() const {
    return data_;
}

std::span<const u8> ByteBuffer::view() const {
    return std::span<const u8>(data_.data() + readPosition_, remaining());
}

void ByteBuffer::need(std::size_t count) const {
    if (count > remaining()) {
        throw BufferUnderflow("buffer underflow");
    }
}

u8 ByteBuffer::readU8() {
    need(1);
    return data_[readPosition_++];
}

i8 ByteBuffer::readI8() {
    return static_cast<i8>(readU8());
}

u16 ByteBuffer::readU16() {
    need(2);
    const u16 value = static_cast<u16>((static_cast<u16>(data_[readPosition_]) << 8)
        | static_cast<u16>(data_[readPosition_ + 1]));
    readPosition_ += 2;
    return value;
}

i16 ByteBuffer::readI16() {
    return static_cast<i16>(readU16());
}

u32 ByteBuffer::readU32() {
    need(4);
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | data_[readPosition_++];
    }
    return value;
}

i32 ByteBuffer::readI32() {
    return std::bit_cast<i32>(readU32());
}

u64 ByteBuffer::readU64() {
    need(8);
    u64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data_[readPosition_++];
    }
    return value;
}

i64 ByteBuffer::readI64() {
    return std::bit_cast<i64>(readU64());
}

float ByteBuffer::readF32() {
    return std::bit_cast<float>(readU32());
}

double ByteBuffer::readF64() {
    return std::bit_cast<double>(readU64());
}

bool ByteBuffer::readBool() {
    return readU8() != 0;
}

i32 ByteBuffer::readVarInt() {
    if (remaining() == 0) {
        throw BufferUnderflow("buffer underflow");
    }
    i32 value = 0;
    const int consumed = varIntDecode(data_.data() + readPosition_, remaining(), value);
    if (consumed > 0) {
        readPosition_ += static_cast<std::size_t>(consumed);
        return value;
    }
    if (consumed == 0) {
        throw BufferUnderflow("buffer underflow");
    }
    throw ProtocolError("varint is too long");
}

i64 ByteBuffer::readVarLong() {
    u64 result = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        const u8 byte = readU8();
        result |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return static_cast<i64>(result);
        }
    }
    throw ProtocolError("varlong is too long");
}

std::string ByteBuffer::readString(std::size_t maxLength) {
    const i32 length = readVarInt();
    if (length < 0 || static_cast<std::size_t>(length) > maxLength) {
        throw ProtocolError("invalid string length");
    }
    const auto count = static_cast<std::size_t>(length);
    need(count);
    std::string value(reinterpret_cast<const char*>(data_.data() + readPosition_), count);
    readPosition_ += count;
    return value;
}

Bytes ByteBuffer::readBytes(std::size_t count) {
    need(count);
    Bytes value(data_.begin() + static_cast<std::ptrdiff_t>(readPosition_),
                data_.begin() + static_cast<std::ptrdiff_t>(readPosition_ + count));
    readPosition_ += count;
    return value;
}

Bytes ByteBuffer::readRemaining() {
    return readBytes(remaining());
}

void ByteBuffer::skip(std::size_t count) {
    need(count);
    readPosition_ += count;
}

void ByteBuffer::writeU8(u8 value) {
    data_.push_back(value);
}

void ByteBuffer::writeI8(i8 value) {
    writeU8(static_cast<u8>(value));
}

void ByteBuffer::writeU16(u16 value) {
    data_.push_back(static_cast<u8>(value >> 8));
    data_.push_back(static_cast<u8>(value & 0xFF));
}

void ByteBuffer::writeI16(i16 value) {
    writeU16(static_cast<u16>(value));
}

void ByteBuffer::writeU32(u32 value) {
    data_.push_back(static_cast<u8>(value >> 24));
    data_.push_back(static_cast<u8>((value >> 16) & 0xFF));
    data_.push_back(static_cast<u8>((value >> 8) & 0xFF));
    data_.push_back(static_cast<u8>(value & 0xFF));
}

void ByteBuffer::writeI32(i32 value) {
    writeU32(std::bit_cast<u32>(value));
}

void ByteBuffer::writeU64(u64 value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        data_.push_back(static_cast<u8>((value >> shift) & 0xFF));
    }
}

void ByteBuffer::writeI64(i64 value) {
    writeU64(std::bit_cast<u64>(value));
}

void ByteBuffer::writeF32(float value) {
    writeU32(std::bit_cast<u32>(value));
}

void ByteBuffer::writeF64(double value) {
    writeU64(std::bit_cast<u64>(value));
}

void ByteBuffer::writeBool(bool value) {
    writeU8(value ? 1 : 0);
}

void ByteBuffer::writeVarInt(i32 value) {
    u8 encoded[5] = {0, 0, 0, 0, 0};
    const int length = varIntEncode(value, encoded);
    data_.insert(data_.end(), encoded, encoded + length);
}

void ByteBuffer::writeVarLong(i64 value) {
    u64 remaining = std::bit_cast<u64>(value);
    while (remaining >= 0x80) {
        writeU8(static_cast<u8>((remaining & 0x7F) | 0x80));
        remaining >>= 7;
    }
    writeU8(static_cast<u8>(remaining));
}

void ByteBuffer::writeString(std::string_view value) {
    writeVarInt(static_cast<i32>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteBuffer::writeBytes(std::span<const u8> value) {
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteBuffer::writePosition(i32 x, i32 y, i32 z) {
    const u64 packed = (static_cast<u64>(x & 0x3FFFFFF) << 38)
        | (static_cast<u64>(y & 0xFFF) << 26)
        | static_cast<u64>(z & 0x3FFFFFF);
    writeU64(packed);
}

void ByteBuffer::readPosition(i32& x, i32& y, i32& z) {
    const u64 packed = readU64();
    x = static_cast<i32>(static_cast<i64>(packed) >> 38);
    y = static_cast<i32>((packed >> 26) & 0xFFF);
    z = static_cast<i32>((static_cast<i64>(packed) << 38) >> 38);
}

void ByteBuffer::clear() {
    data_.clear();
    readPosition_ = 0;
}

void ByteBuffer::compact() {
    if (readPosition_ == 0) {
        return;
    }
    if (readPosition_ >= data_.size()) {
        data_.clear();
        readPosition_ = 0;
        return;
    }
    data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(readPosition_));
    readPosition_ = 0;
}

Bytes ByteBuffer::release() {
    readPosition_ = 0;
    return std::move(data_);
}

}
