// The hashes and ciphers the launcher needs: CNG on Windows (crypto_win.cpp), OpenSSL's libcrypto elsewhere
// (crypto_openssl.cpp). Every function returns false on failure.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dcp::crypto {

bool Sha1(const uint8_t* data, size_t size, uint8_t (&digest)[20]);
bool Sha256(const uint8_t* data, size_t size, uint8_t (&digest)[32]);
bool HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* data, size_t size, uint8_t (&digest)[32]);
bool HmacSha1(const uint8_t* key, size_t key_size, const uint8_t* data, size_t size, uint8_t (&digest)[20]);

class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    bool Update(const void* data, size_t size);
    bool Finish(uint8_t (&digest)[32]);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class AesMode { kEcb, kCbc };

// AES-128, in place; sizes are whole blocks. A CBC IV is consumed: its contents afterwards are unspecified.
class AesKey {
public:
    AesKey();
    ~AesKey();
    AesKey(const AesKey&) = delete;
    AesKey& operator=(const AesKey&) = delete;
    bool Init(AesMode mode, const uint8_t* key);
    bool Encrypt(uint8_t* data, size_t size, uint8_t* iv = nullptr);
    bool Decrypt(uint8_t* data, size_t size, uint8_t* iv = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// RSAES-PKCS1-v1_5 with a 2048-bit private key given by its big-endian modulus (256 bytes) and primes (128 bytes
// each; public exponent 65537); the message without its padding.
bool RsaDecryptPkcs1(const uint8_t* modulus, const uint8_t* prime1, const uint8_t* prime2, const uint8_t* cipher,
                     std::vector<uint8_t>& message);

} // namespace dcp::crypto
