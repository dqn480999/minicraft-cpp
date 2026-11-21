#include "common/Crypto.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <mutex>

#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "common/Types.hpp"

namespace mc {
namespace {

std::mutex g_cryptoMutex;

Bytes digest(const EVP_MD* algorithm, std::span<const u8> data) {
    std::lock_guard lock(g_cryptoMutex);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        throw ProtocolError("failed to allocate digest context");
    }
    Bytes output(static_cast<std::size_t>(EVP_MD_get_size(algorithm)));
    unsigned int length = 0;
    const bool ok = EVP_DigestInit_ex(context, algorithm, nullptr) == 1
        && EVP_DigestUpdate(context, data.data(), data.size()) == 1
        && EVP_DigestFinal_ex(context, output.data(), &length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        throw ProtocolError("digest calculation failed");
    }
    output.resize(length);
    return output;
}

void processCipher(EVP_CIPHER_CTX* context, u8* data, std::size_t size, bool encrypting) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size - offset, static_cast<std::size_t>(INT_MAX)));
        int produced = 0;
        const int ok = encrypting
            ? EVP_EncryptUpdate(context, data + offset, &produced, data + offset, chunk)
            : EVP_DecryptUpdate(context, data + offset, &produced, data + offset, chunk);
        if (ok != 1 || produced != chunk) {
            throw ProtocolError("aes stream processing failed");
        }
        offset += static_cast<std::size_t>(produced);
    }
}

}

Bytes md5(std::span<const u8> data) {
    return digest(EVP_md5(), data);
}

Bytes sha1(std::span<const u8> data) {
    return digest(EVP_sha1(), data);
}

Bytes randomBytes(std::size_t count) {
    std::lock_guard lock(g_cryptoMutex);
    Bytes output(count);
    if (count > 0 && RAND_bytes(output.data(), static_cast<int>(count)) != 1) {
        throw ProtocolError("random generation failed");
    }
    return output;
}

std::string hexString(std::span<const u8> data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(data.size() * 2);
    for (const u8 byte : data) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0F]);
    }
    return output;
}

RsaKey::RsaKey() {
    std::lock_guard lock(g_cryptoMutex);
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (context == nullptr) {
        throw ProtocolError("failed to allocate rsa context");
    }
    const bool ok = EVP_PKEY_keygen_init(context) == 1
        && EVP_PKEY_CTX_set_rsa_keygen_bits(context, 1024) == 1
        && EVP_PKEY_keygen(context, &key_) == 1;
    EVP_PKEY_CTX_free(context);
    if (!ok || key_ == nullptr) {
        throw ProtocolError("failed to generate rsa key");
    }
}

RsaKey::~RsaKey() {
    if (key_ != nullptr) {
        EVP_PKEY_free(key_);
    }
}

RsaKey::RsaKey(RsaKey&& other) noexcept : key_(other.key_) {
    other.key_ = nullptr;
}

RsaKey& RsaKey::operator=(RsaKey&& other) noexcept {
    if (this != &other) {
        if (key_ != nullptr) {
            EVP_PKEY_free(key_);
        }
        key_ = other.key_;
        other.key_ = nullptr;
    }
    return *this;
}

bool RsaKey::valid() const {
    return key_ != nullptr;
}

Bytes RsaKey::publicKeyDer() const {
    if (key_ == nullptr) {
        throw ProtocolError("rsa key is not initialized");
    }
    std::lock_guard lock(g_cryptoMutex);
    unsigned char* buffer = nullptr;
    const int length = i2d_PUBKEY(key_, &buffer);
    if (length <= 0 || buffer == nullptr) {
        throw ProtocolError("failed to encode public key");
    }
    Bytes output(buffer, buffer + length);
    OPENSSL_free(buffer);
    return output;
}

Bytes RsaKey::decrypt(std::span<const u8> ciphertext) const {
    if (key_ == nullptr) {
        throw ProtocolError("rsa key is not initialized");
    }
    std::lock_guard lock(g_cryptoMutex);
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new(key_, nullptr);
    if (context == nullptr) {
        throw ProtocolError("failed to allocate rsa decrypt context");
    }
    std::size_t length = 0;
    bool ok = EVP_PKEY_decrypt_init(context) == 1
        && EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) == 1
        && EVP_PKEY_decrypt(context, nullptr, &length, ciphertext.data(), ciphertext.size()) == 1;
    Bytes output;
    if (ok) {
        output.resize(length);
        ok = EVP_PKEY_decrypt(context, output.data(), &length, ciphertext.data(), ciphertext.size()) == 1;
        output.resize(length);
    }
    EVP_PKEY_CTX_free(context);
    if (!ok) {
        throw ProtocolError("rsa decryption failed");
    }
    return output;
}

AesCfb8::AesCfb8(std::span<const u8> key) {
    if (key.size() != 16) {
        throw ProtocolError("aes key must be 16 bytes");
    }
    std::lock_guard lock(g_cryptoMutex);
    encrypt_ = EVP_CIPHER_CTX_new();
    decrypt_ = EVP_CIPHER_CTX_new();
    if (encrypt_ == nullptr || decrypt_ == nullptr) {
        throw ProtocolError("failed to allocate aes context");
    }
    const bool ok = EVP_EncryptInit_ex(encrypt_, EVP_aes_128_cfb8(), nullptr, key.data(), key.data()) == 1
        && EVP_DecryptInit_ex(decrypt_, EVP_aes_128_cfb8(), nullptr, key.data(), key.data()) == 1;
    if (!ok) {
        throw ProtocolError("failed to initialize aes");
    }
    EVP_CIPHER_CTX_set_padding(encrypt_, 0);
    EVP_CIPHER_CTX_set_padding(decrypt_, 0);
}

AesCfb8::~AesCfb8() {
    if (encrypt_ != nullptr) {
        EVP_CIPHER_CTX_free(encrypt_);
    }
    if (decrypt_ != nullptr) {
        EVP_CIPHER_CTX_free(decrypt_);
    }
}

void AesCfb8::encrypt(u8* data, std::size_t size) {
    processCipher(encrypt_, data, size, true);
}

void AesCfb8::decrypt(u8* data, std::size_t size) {
    processCipher(decrypt_, data, size, false);
}

std::string minecraftServerHash(std::string_view serverId, std::span<const u8> sharedSecret, std::span<const u8> publicKey) {
    Bytes input;
    input.reserve(serverId.size() + sharedSecret.size() + publicKey.size());
    input.insert(input.end(), serverId.begin(), serverId.end());
    input.insert(input.end(), sharedSecret.begin(), sharedSecret.end());
    input.insert(input.end(), publicKey.begin(), publicKey.end());

    std::array<u8, 20> value{};
    const auto hash = sha1(input);
    std::copy(hash.begin(), hash.end(), value.begin());

    const bool negative = (value[0] & 0x80) != 0;
    if (negative) {
        int carry = 1;
        for (int i = 19; i >= 0; --i) {
            const int sum = static_cast<int>(static_cast<u8>(~value[i])) + carry;
            value[i] = static_cast<u8>(sum & 0xFF);
            carry = sum >> 8;
        }
    }

    std::string output;
    output.reserve(41);
    if (negative) {
        output.push_back('-');
    }
    bool leading = true;
    for (const u8 byte : value) {
        const char high = "0123456789abcdef"[byte >> 4];
        const char low = "0123456789abcdef"[byte & 0x0F];
        if (leading && high == '0' && low == '0') {
            continue;
        }
        if (leading && high == '0') {
            output.push_back(low);
            leading = false;
            continue;
        }
        leading = false;
        output.push_back(high);
        output.push_back(low);
    }
    if (output.empty() || output == "-") {
        output = "0";
    }
    return output;
}

}
