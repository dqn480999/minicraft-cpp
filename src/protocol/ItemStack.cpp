#include "protocol/ItemStack.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "net/ByteBuffer.hpp"

namespace mc {
namespace {

constexpr int kMaxNbtDepth = 64;
constexpr i32 kMaxNbtArray = 16 * 1024 * 1024;

void skipNbtPayload(ByteBuffer& buffer, u8 type, int depth);

void skipNbtPayload(ByteBuffer& buffer, u8 type, int depth) {
    if (depth > kMaxNbtDepth) {
        throw ProtocolError("nbt nesting is too deep");
    }
    switch (type) {
    case 1:
        buffer.skip(1);
        break;
    case 2:
        buffer.skip(2);
        break;
    case 3:
        buffer.skip(4);
        break;
    case 4:
        buffer.skip(8);
        break;
    case 5:
        buffer.skip(4);
        break;
    case 6:
        buffer.skip(8);
        break;
    case 7: {
        const i32 length = buffer.readI32();
        if (length < 0 || length > kMaxNbtArray) {
            throw ProtocolError("invalid nbt byte array");
        }
        buffer.skip(static_cast<std::size_t>(length));
        break;
    }
    case 8: {
        const u16 length = buffer.readU16();
        buffer.skip(length);
        break;
    }
    case 9: {
        const u8 elementType = buffer.readU8();
        const i32 length = buffer.readI32();
        if (length < 0 || length > kMaxNbtArray) {
            throw ProtocolError("invalid nbt list");
        }
        for (i32 i = 0; i < length; ++i) {
            skipNbtPayload(buffer, elementType, depth + 1);
        }
        break;
    }
    case 10:
        while (true) {
            const u8 childType = buffer.readU8();
            if (childType == 0) {
                break;
            }
            const u16 nameLength = buffer.readU16();
            buffer.skip(nameLength);
            skipNbtPayload(buffer, childType, depth + 1);
        }
        break;
    case 11: {
        const i32 length = buffer.readI32();
        if (length < 0 || length > kMaxNbtArray) {
            throw ProtocolError("invalid nbt int array");
        }
        buffer.skip(static_cast<std::size_t>(length) * 4);
        break;
    }
    case 12: {
        const i32 length = buffer.readI32();
        if (length < 0 || length > kMaxNbtArray) {
            throw ProtocolError("invalid nbt long array");
        }
        buffer.skip(static_cast<std::size_t>(length) * 8);
        break;
    }
    default:
        throw ProtocolError("unsupported nbt tag type");
    }
}

}

int maxStackSize(i16 itemId) {
    if (itemId < 0) {
        return 64;
    }
    if ((itemId >= 256 && itemId <= 259) || itemId == 261
        || (itemId >= 267 && itemId <= 279) || itemId == 282
        || (itemId >= 283 && itemId <= 286) || (itemId >= 290 && itemId <= 294)
        || (itemId >= 298 && itemId <= 317) || itemId == 326 || itemId == 327
        || itemId == 328 || itemId == 329 || itemId == 333 || itemId == 335
        || itemId == 342 || itemId == 343 || itemId == 346 || itemId == 354
        || itemId == 355 || itemId == 359 || itemId == 373 || itemId == 386
        || itemId == 398 || itemId == 403 || itemId == 407 || itemId == 408
        || itemId == 413 || (itemId >= 417 && itemId <= 419) || itemId == 422
        || itemId >= 2256) {
        return 1;
    }
    if (itemId == 323 || itemId == 325 || itemId == 332 || itemId == 344
        || itemId == 368 || itemId == 387 || itemId == 416 || itemId == 425) {
        return 16;
    }
    return 64;
}

bool isValidItemId(i16 itemId) {
    if (itemId < 0 || itemId > 2267) {
        return false;
    }
    static constexpr std::array<std::uint64_t, 36> bits = {
        0x3777f7ebfbfff0feULL, 0x4f17fcff8bf7f36eULL, 0xffd8ffffff9e8fefULL, 0x0000000000000001ULL,
        0xffffffffffffffffULL, 0xffffffffffffffffULL, 0x0000fbffffffffffULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
        0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x000000000fff0000ULL,
    };
    const auto word = static_cast<std::size_t>(itemId) / 64;
    const auto bit = static_cast<unsigned>(itemId) % 64;
    return ((bits[word] >> bit) & 1ULL) != 0;
}

ItemStack sanitizeItemStack(ItemStack stack) {
    if (stack.empty() || !isValidItemId(stack.id)) {
        return ItemStack{};
    }
    stack.count = static_cast<i8>(std::clamp<int>(stack.count, 1, maxStackSize(stack.id)));
    return stack;
}

void writeItemStack(ByteBuffer& buffer, const ItemStack& stack) {
    const ItemStack safe = sanitizeItemStack(stack);
    if (safe.empty()) {
        buffer.writeI16(-1);
        return;
    }
    buffer.writeI16(safe.id);
    buffer.writeI8(safe.count);
    buffer.writeI16(safe.damage);
    buffer.writeU8(0);
}

ItemStack readItemStack(ByteBuffer& buffer) {
    const i16 id = buffer.readI16();
    if (id < 0) {
        return {};
    }
    ItemStack stack;
    stack.id = id;
    stack.count = buffer.readI8();
    stack.damage = buffer.readI16();
    const u8 nbtType = buffer.readU8();
    if (nbtType != 0) {
        const u16 nameLength = buffer.readU16();
        buffer.skip(nameLength);
        skipNbtPayload(buffer, nbtType, 0);
    }
    return sanitizeItemStack(stack);
}

}
