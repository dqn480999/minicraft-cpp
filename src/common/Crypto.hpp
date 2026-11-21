#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <openssl/evp.h>

#include "common/Types.hpp"

namespace mc {

Bytes md5(std::span<const u8> data);
Bytes sha1(std::span<const u8> data);
Bytes randomBytes(std::size_t count);
std::string hexString(std::span<const u8> data);

class RsaKey {
public:
    RsaKey();
    ~RsaKey();
    RsaKey(const RsaKey&) = delete;
    RsaKey& operator=(const RsaKey&) = delete;
    RsaKey(RsaKey&& other) noexcept;
    RsaKey& operator=(RsaKey&& other) noexcept;

    bool valid() const;
    Bytes publicKeyDer() const;
    Bytes decrypt(std::span<const u8> ciphertext) const;

private:
    EVP_PKEY* key_{nullptr};
};

class AesCfb8 {
public:
    explicit AesCfb8(std::span<const u8> key);
    ~AesCfb8();
    AesCfb8(const AesCfb8&) = delete;
    AesCfb8& operator=(const AesCfb8&) = delete;

    void encrypt(u8* data, std::size_t size);
    void decrypt(u8* data, std::size_t size);

private:
    EVP_CIPHER_CTX* encrypt_{nullptr};
    EVP_CIPHER_CTX* decrypt_{nullptr};
};

std::string minecraftServerHash(std::string_view serverId, std::span<const u8> sharedSecret, std::span<const u8> publicKey);

}
