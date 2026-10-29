// Random bytes from the system and SHA-256 / HMAC-SHA-256, for the identity and the peer handshake.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dcp::net {

using Digest = std::array<uint8_t, 32>;

// False only when the system generator fails.
bool random_bytes(void* out, size_t size);
uint64_t random_u64();

Digest sha256(const void* data, size_t size);
Digest hmac_sha256(const void* key, size_t key_size, const void* data, size_t size);

} // namespace dcp::net
