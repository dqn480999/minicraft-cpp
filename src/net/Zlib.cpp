#include "net/Zlib.hpp"

#include <limits>

#include <zlib.h>

namespace mc {

Bytes zlibCompress(std::span<const u8> data) {
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    Bytes output(bound);
    const int result = compress2(output.data(), &bound, data.data(), static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION);
    if (result != Z_OK) {
        throw ProtocolError("zlib compression failed");
    }
    output.resize(bound);
    return output;
}

Bytes zlibDecompress(std::span<const u8> data, std::size_t expectedSize, std::size_t maxSize) {
    if (expectedSize > maxSize || expectedSize > 512ULL * 1024ULL * 1024ULL) {
        throw ProtocolError("decompressed packet is too large");
    }
    if (expectedSize == 0) {
        return {};
    }
    Bytes output(expectedSize);
    uLongf length = static_cast<uLongf>(expectedSize);
    const int result = uncompress(output.data(), &length, data.data(), static_cast<uLong>(data.size()));
    if (result != Z_OK || length != expectedSize) {
        throw ProtocolError("zlib decompression failed");
    }
    output.resize(length);
    return output;
}

}
