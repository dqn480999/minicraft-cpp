#pragma once

#include <cstddef>
#include <mutex>
#include <span>

#include "common/Crypto.hpp"
#include "common/Types.hpp"

namespace mc {

class Encryption {
public:
    explicit Encryption(std::span<const u8> sharedSecret);

    void encrypt(u8* data, std::size_t size);
    void decrypt(u8* data, std::size_t size);

private:
    AesCfb8 cipher_;
    std::mutex encryptMutex_;
    std::mutex decryptMutex_;
};

}
