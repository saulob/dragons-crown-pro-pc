#include <algorithm>
#include <cstring>

#include "crypto.h"

namespace dcp::crypto {

bool HmacSha1(const uint8_t* key, size_t key_size, const uint8_t* data, size_t size, uint8_t (&digest)[20]) {
    constexpr size_t kBlock = 64;
    uint8_t block[kBlock] = {};
    if (key_size > kBlock) {
        uint8_t hashed[20];
        if (!Sha1(key, key_size, hashed)) return false;
        std::memcpy(block, hashed, sizeof(hashed));
    } else if (key_size > 0) {
        std::memcpy(block, key, key_size);
    }
    std::vector<uint8_t> inner(kBlock + size);
    for (size_t i = 0; i < kBlock; ++i) inner[i] = block[i] ^ 0x36;
    if (size > 0) std::memcpy(inner.data() + kBlock, data, size);
    uint8_t inner_digest[20];
    if (!Sha1(inner.data(), inner.size(), inner_digest)) return false;

    uint8_t outer[kBlock + 20];
    for (size_t i = 0; i < kBlock; ++i) outer[i] = block[i] ^ 0x5c;
    std::memcpy(outer + kBlock, inner_digest, sizeof(inner_digest));
    return Sha1(outer, sizeof(outer), digest);
}

} // namespace dcp::crypto
