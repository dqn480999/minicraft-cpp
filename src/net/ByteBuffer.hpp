#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/Types.hpp"

namespace mc {

class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(Bytes data);

    void reserve(std::size_t capacity);
    void append(const void* data, std::size_t size);
    void append(std::span<const u8> data);
    void append(const Bytes& data);
    void append(const ByteBuffer& other);

    std::size_t size() const;
    std::size_t remaining() const;
    std::size_t readPosition() const;
    void setReadPosition(std::size_t position);
    const Bytes& data() const;
    std::span<const u8> view() const;

    u8 readU8();
    i8 readI8();
    u16 readU16();
    i16 readI16();
    u32 readU32();
    i32 readI32();
    u64 readU64();
    i64 readI64();
    float readF32();
    double readF64();
    bool readBool();
    i32 readVarInt();
    i64 readVarLong();
    std::string readString(std::size_t maxLength = kMaxStringLength);
    Bytes readBytes(std::size_t count);
    Bytes readRemaining();
    void skip(std::size_t count);

    void writeU8(u8 value);
    void writeI8(i8 value);
    void writeU16(u16 value);
    void writeI16(i16 value);
    void writeU32(u32 value);
    void writeI32(i32 value);
    void writeU64(u64 value);
    void writeI64(i64 value);
    void writeF32(float value);
    void writeF64(double value);
    void writeBool(bool value);
    void writeVarInt(i32 value);
    void writeVarLong(i64 value);
    void writeString(std::string_view value);
    void writeBytes(std::span<const u8> value);
    void writePosition(i32 x, i32 y, i32 z);
    void readPosition(i32& x, i32& y, i32& z);

    void clear();
    void compact();
    Bytes release();

private:
    void need(std::size_t count) const;
    Bytes data_;
    std::size_t readPosition_{0};
};

}
