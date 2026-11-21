#include "net/Encryption.hpp"

namespace mc {

Encryption::Encryption(std::span<const u8> sharedSecret) : cipher_(sharedSecret) {}

void Encryption::encrypt(u8* data, std::size_t size) {
    std::lock_guard lock(encryptMutex_);
    cipher_.encrypt(data, size);
}

void Encryption::decrypt(u8* data, std::size_t size) {
    std::lock_guard lock(decryptMutex_);
    cipher_.decrypt(data, size);
}

}
