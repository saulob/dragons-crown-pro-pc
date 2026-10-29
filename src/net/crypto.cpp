#include "net/crypto.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace dcp::net {
namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

class Sha256 {
public:
    void Update(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        total_ += size;
        while (size) {
            const size_t take = std::min(size, size_t(64) - used_);
            std::memcpy(block_ + used_, p, take);
            used_ += take, p += take, size -= take;
            if (used_ == 64) Compress(), used_ = 0;
        }
    }

    Digest Finish() {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        Update(&one, 1);
        while (used_ != 56) Update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
        Update(length, 8);
        Digest out;
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) out[size_t(i * 4 + b)] = uint8_t(h_[i] >> (24 - 8 * b));
        return out;
    }

private:
    void Compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(block_[i * 4]) << 24 | uint32_t(block_[i * 4 + 1]) << 16 | uint32_t(block_[i * 4 + 2]) << 8 |
                   block_[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }

    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block_[64] = {};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

} // namespace

bool random_bytes(void* out, size_t size) {
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    auto* p = static_cast<uint8_t*>(out);
    while (size) {
        const ssize_t n = getrandom(p, size, 0);
        if (n <= 0) return false;
        p += n, size -= size_t(n);
    }
    return true;
#endif
}

uint64_t random_u64() {
    uint64_t v = 0;
    random_bytes(&v, sizeof(v));
    return v;
}

Digest sha256(const void* data, size_t size) {
    Sha256 s;
    s.Update(data, size);
    return s.Finish();
}

Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t size) {
    uint8_t block[64] = {};
    if (key_size > 64) {
        const Digest k = sha256(key, key_size);
        std::memcpy(block, k.data(), k.size());
    } else {
        std::memcpy(block, key, key_size);
    }
    uint8_t inner_pad[64], outer_pad[64];
    for (int i = 0; i < 64; ++i) inner_pad[i] = block[i] ^ 0x36, outer_pad[i] = block[i] ^ 0x5c;
    Sha256 inner;
    inner.Update(inner_pad, 64);
    inner.Update(data, size);
    const Digest first = inner.Finish();
    Sha256 outer;
    outer.Update(outer_pad, 64);
    outer.Update(first.data(), first.size());
    return outer.Finish();
}

} // namespace dcp::net
