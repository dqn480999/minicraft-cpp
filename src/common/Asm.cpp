#include "common/Asm.hpp"

#include <cstring>

#ifdef MINICRAFT_HAVE_NASM
extern "C" {
void mc_fast_copy(void* destination, const void* source, std::size_t size);
void mc_fast_zero(void* destination, std::size_t size);
int mc_varint_decode(const mc::u8* data, std::size_t size, mc::i32* value);
int mc_varint_encode(mc::i32 value, mc::u8* output);
void mc_fast_copy_avx2(void* destination, const void* source, std::size_t size);
void mc_fast_zero_avx2(void* destination, std::size_t size);
}

namespace {

bool avx2Available() {
#if defined(__x86_64__) || defined(__i386__)
    static const bool available = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2");
    }();
    return available;
#else
    return false;
#endif
}

}
#endif

namespace mc {

bool assemblyAvailable() {
#ifdef MINICRAFT_HAVE_NASM
    return true;
#else
    return false;
#endif
}

void fastCopy(void* destination, const void* source, std::size_t size) {
#ifdef MINICRAFT_HAVE_NASM
    if (size >= 128 && avx2Available()) {
        mc_fast_copy_avx2(destination, source, size);
    } else {
        mc_fast_copy(destination, source, size);
    }
#else
    std::memcpy(destination, source, size);
#endif
}

void fastZero(void* destination, std::size_t size) {
#ifdef MINICRAFT_HAVE_NASM
    if (size >= 128 && avx2Available()) {
        mc_fast_zero_avx2(destination, size);
    } else {
        mc_fast_zero(destination, size);
    }
#else
    std::memset(destination, 0, size);
#endif
}

int varIntDecode(const u8* data, std::size_t size, i32& value) {
#ifdef MINICRAFT_HAVE_NASM
    return mc_varint_decode(data, size, &value);
#else
    u32 result = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        if (i >= size) {
            return 0;
        }
        const u8 byte = data[i];
        result |= static_cast<u32>(byte & 0x7F) << (7 * i);
        if ((byte & 0x80) == 0) {
            value = static_cast<i32>(result);
            return static_cast<int>(i + 1);
        }
    }
    return -1;
#endif
}

int varIntEncode(i32 value, u8* output) {
#ifdef MINICRAFT_HAVE_NASM
    return mc_varint_encode(value, output);
#else
    u32 remaining = static_cast<u32>(value);
    int length = 0;
    while (remaining >= 0x80) {
        output[length++] = static_cast<u8>((remaining & 0x7F) | 0x80);
        remaining >>= 7;
    }
    output[length++] = static_cast<u8>(remaining);
    return length;
#endif
}

}
