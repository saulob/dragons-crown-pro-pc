// SPDX-License-Identifier: GPL-2.0-or-later
// dcp: the Vulkan-independent rules of the persistent pipeline cache: blob integrity, cache
// identity, the warm-up mode and what a failed pipeline creation leads to.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace PipelineCachePolicy {

inline std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ull;
    }
    return hash;
}

namespace Detail {
template <typename T>
void Append(std::vector<std::uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
template <typename T>
T At(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
} // namespace Detail

constexpr std::uint32_t SealMagic = 0x42504344; // "DCPB"
constexpr std::size_t SealSize = 16;

/// Appends magic, payload size and checksum: a truncated, zero-filled or foreign file never
/// passes Unseal.
inline void Seal(std::vector<std::uint8_t>& bytes) {
    const auto size = std::uint32_t(bytes.size());
    const auto hash = Fnv1a(bytes);
    Detail::Append(bytes, SealMagic);
    Detail::Append(bytes, size);
    Detail::Append(bytes, hash);
}

/// Strips the seal; false (bytes untouched) when the blob is not intact.
inline bool Unseal(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < SealSize) {
        return false;
    }
    const std::size_t payload = bytes.size() - SealSize;
    const std::span<const std::uint8_t> all{bytes};
    if (Detail::At<std::uint32_t>(all, payload) != SealMagic ||
        Detail::At<std::uint32_t>(all, payload + 4) != payload ||
        Detail::At<std::uint64_t>(all, payload + 8) != Fnv1a(all.first(payload))) {
        return false;
    }
    bytes.resize(payload);
    return true;
}

inline bool IsTempFile(const std::filesystem::path& path) {
    return path.extension() == ".tmp";
}

/// Writes beside the target and renames into place: a process that ends at any point leaves
/// either the previous complete file or the new one, plus at most a stray .tmp.
inline bool WriteFileAtomically(const std::filesystem::path& file,
                                std::span<const std::uint8_t> bytes) {
    auto temp = file;
    temp += ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        stream.close();
        if (stream.fail()) {
            std::error_code error;
            std::filesystem::remove(temp, error);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, file, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

/// The GPU and driver the cache was written on.
struct GpuIdentity {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t driver_version{};
    std::uint32_t api_version{};
    bool operator==(const GpuIdentity&) const = default;
};

/// What the shader/pipeline store depends on. The store holds SPIR-V and pipeline state, not
/// driver binaries, so a driver update alone keeps it.
struct StoreIdentity {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t shader_binary_version{};
    std::uint32_t shader_meta_version{};
    std::uint32_t pipeline_key_version{};
    bool operator==(const StoreIdentity&) const = default;
};

constexpr std::uint32_t StoreIdentityMagic = 0x49504344; // "DCPI"
constexpr std::uint32_t WarmupPolicyMagic = 0x57504344;  // "DCPW"
constexpr std::uint32_t RecordVersion = 1;

namespace Detail {
template <typename T>
std::vector<std::uint8_t> EncodeRecord(std::uint32_t magic, const T& value) {
    std::vector<std::uint8_t> bytes;
    Append(bytes, magic);
    Append(bytes, RecordVersion);
    Append(bytes, std::uint32_t(sizeof(T)));
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
    return bytes;
}
template <typename T>
std::optional<T> DecodeRecord(std::uint32_t magic, std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 12 + sizeof(T) || At<std::uint32_t>(bytes, 0) != magic ||
        At<std::uint32_t>(bytes, 4) != RecordVersion || At<std::uint32_t>(bytes, 8) != sizeof(T)) {
        return std::nullopt;
    }
    return At<T>(bytes, 12);
}
} // namespace Detail

inline std::vector<std::uint8_t> EncodeStoreIdentity(const StoreIdentity& identity) {
    return Detail::EncodeRecord(StoreIdentityMagic, identity);
}
inline std::optional<StoreIdentity> DecodeStoreIdentity(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<StoreIdentity>(StoreIdentityMagic, bytes);
}

enum class StoreVerdict { Reuse, Missing, Corrupt, Incompatible };

inline StoreVerdict CheckStore(std::span<const std::uint8_t> stored, const StoreIdentity& current) {
    if (stored.empty()) {
        return StoreVerdict::Missing;
    }
    const auto identity = DecodeStoreIdentity(stored);
    if (!identity) {
        return StoreVerdict::Corrupt;
    }
    return *identity == current ? StoreVerdict::Reuse : StoreVerdict::Incompatible;
}

inline std::string_view ToString(StoreVerdict verdict) {
    switch (verdict) {
    case StoreVerdict::Reuse:
        return "compatible";
    case StoreVerdict::Missing:
        return "no identity stamp";
    case StoreVerdict::Corrupt:
        return "corrupt identity stamp";
    case StoreVerdict::Incompatible:
        return "written on another GPU or by another cache version";
    }
    return "unknown";
}

/// Driver-dependent: recorded after a pipeline failed while the warm-up built in parallel.
struct WarmupPolicy {
    GpuIdentity gpu{};
    std::uint32_t serial_builds{};
};

inline std::vector<std::uint8_t> EncodeWarmupPolicy(const WarmupPolicy& policy) {
    return Detail::EncodeRecord(WarmupPolicyMagic, policy);
}
inline std::optional<WarmupPolicy> DecodeWarmupPolicy(std::span<const std::uint8_t> bytes) {
    return Detail::DecodeRecord<WarmupPolicy>(WarmupPolicyMagic, bytes);
}

enum class WarmupMode { Parallel, Serial };

/// Serial only on the exact GPU and driver that failed; a driver update gets parallel back.
inline WarmupMode ChooseWarmupMode(std::span<const std::uint8_t> stored, const GpuIdentity& gpu) {
    const auto policy = DecodeWarmupPolicy(stored);
    return policy && policy->serial_builds && policy->gpu == gpu ? WarmupMode::Serial
                                                                 : WarmupMode::Parallel;
}

/// Pipelines build in parallel until EnableSerial, then one at a time.
class BuildGate {
public:
    class Slot {
    public:
        explicit Slot(BuildGate& gate_) : gate{gate_}, lock{gate_.mutex, std::defer_lock} {
            // Counted before serial is read, so WaitForParallelBuilds sees every build that
            // started without the lock.
            ++gate.parallel;
            if (gate.serial) {
                --gate.parallel;
                lock.lock();
            }
        }
        ~Slot() {
            if (!lock.owns_lock()) {
                --gate.parallel;
            }
        }
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        bool Parallel() const {
            return !lock.owns_lock();
        }

    private:
        BuildGate& gate;
        std::unique_lock<std::mutex> lock;
    };

    /// True for the call that switched to serial builds.
    bool EnableSerial() {
        return !serial.exchange(true);
    }
    bool Serial() const {
        return serial;
    }
    void WaitForParallelBuilds() const {
        while (parallel.load() != 0) {
            std::this_thread::yield();
        }
    }

private:
    std::atomic<bool> serial{false};
    std::atomic<unsigned> parallel{0};
    std::mutex mutex;
};

enum class PipelineOrigin { Preload, Game };
enum class FailureAction { DropCachedEntry, RetryWithWarmUpPaused, Fatal };

/// After a creation already retried without the driver pipeline cache failed again. A preloaded
/// pipeline is only an optimisation; the game's own gets one more try with the warm-up paused.
constexpr FailureAction OnPipelineFailure(PipelineOrigin origin, bool game_retried) {
    if (origin == PipelineOrigin::Preload) {
        return FailureAction::DropCachedEntry;
    }
    return game_retried ? FailureAction::Fatal : FailureAction::RetryWithWarmUpPaused;
}

} // namespace PipelineCachePolicy
