// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <xxhash.h>

#include "video_core/renderer_opengl/gl_guest_watch.h"

namespace OpenGL {
namespace {

constexpr u64 FullHashLimit = 256 * 1024;
constexpr u64 SampleBytes = 64 * 1024;
constexpr u32 MaxFaultRetries = 256;
/// A stuck fault spins through the limit in milliseconds; rare harmless retries must never add up.
constexpr std::chrono::seconds RetryWindow{1};

constinit thread_local u64 retried_page = ~0ull;
constinit thread_local u32 retry_count = 0;
constinit thread_local std::chrono::steady_clock::rep retry_window_start = 0;

} // namespace

u64 HashGuestRange(const u8* data, u64 size) {
    if (size <= FullHashLimit) {
        return XXH3_64bits(data, size);
    }
    u64 hash = XXH3_64bits(data, SampleBytes);
    hash = XXH3_64bits_withSeed(data + (size - SampleBytes) / 2, SampleBytes, hash);
    return XXH3_64bits_withSeed(data + size - SampleBytes, SampleBytes, hash);
}

bool RetryFault(VAddr address) {
    using Clock = std::chrono::steady_clock;
    const u64 page = address >> 12;
    const auto now = Clock::now().time_since_epoch();
    const auto window = std::chrono::duration_cast<Clock::duration>(RetryWindow).count();
    if (page != retried_page || now.count() - retry_window_start > window) {
        retried_page = page;
        retry_count = 0;
        retry_window_start = now.count();
    }
    return ++retry_count <= MaxFaultRetries;
}

void ResetFaultRetries() {
    retried_page = ~0ull;
    retry_count = 0;
}

GuestWriteWatch::GuestWriteWatch(ProtectFunction protect_, u32 cooldown_frames_)
    : protect{protect_}, cooldown_frames{cooldown_frames_},
      leaves{std::make_unique<std::atomic<Leaf*>[]>(LeafCount)} {}

GuestWriteWatch::~GuestWriteWatch() {
    ReleaseAll();
    for (u32 i = 0; i < LeafCount; ++i) {
        delete leaves[i].load(std::memory_order_relaxed);
    }
}

void GuestWriteWatch::AdvanceFrame() {
    frame.fetch_add(1, std::memory_order_relaxed);
}

void GuestWriteWatch::ReleaseRun::Add(u64 page) {
    if (end != page) {
        Flush();
        begin = page;
    }
    end = page + 1;
}

void GuestWriteWatch::ReleaseRun::Flush() {
    if (end != begin) {
        watch.protect(begin << PageBits, (end - begin) << PageBits, true);
    }
    begin = end = 0;
}

void GuestWriteWatch::Disarm(Leaf& leaf, u32 index) {
    leaf.state[index] = Writable;
}

void GuestWriteWatch::RecordWrite(Leaf& leaf, u32 index, u32 written) {
    const u32 now = frame.load(std::memory_order_relaxed);
    const u32 last = leaf.last_write_frame[index];
    leaf.written[index].store(written, std::memory_order_relaxed);
    if (last == now) {
        return;
    }
    u8& strikes = leaf.strikes[index];
    const bool repeated = last != 0 && now - last <= u64(Cooldown(strikes)) * 2;
    strikes = repeated ? std::min<u8>(strikes + 1, MaxStrikes) : 0;
    leaf.last_write_frame[index] = now;
    leaf.cool_until[index] = now + Cooldown(strikes);
}

void GuestWriteWatch::RecordNeighbor(Leaf& leaf, u32 index, u32 written) {
    const u32 now = frame.load(std::memory_order_relaxed);
    leaf.written[index].store(written, std::memory_order_relaxed);
    leaf.cool_until[index] = std::max(leaf.cool_until[index], now + cooldown_frames);
}

// Pages are protected under the lock so a fault cannot undo an arm it raced with. Nothing under
// the lock touches guest memory, so the fault handler can never wait for its own thread.
std::optional<u64> GuestWriteWatch::TryArm(VAddr address, u64 size) {
    if (size == 0 || address >= AddressLimit || size > AddressLimit - address) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    const u32 now = frame.load(std::memory_order_relaxed);
    const u64 first = address >> PageBits;
    const u64 end = (address + size + PageBytes - 1) >> PageBits;
    for (u64 page = first; page < end; ++page) {
        const Leaf* leaf = LeafOf(page);
        if (leaf && leaf->cool_until[page & (PagesPerLeaf - 1)] > now) {
            return std::nullopt;
        }
    }
    const u64 stamp = epoch.load(std::memory_order_relaxed);
    for (u64 page = first; page < end; ++page) {
        Leaf* leaf = LeafOf(page);
        if (!leaf) {
            leaf = new Leaf{};
            leaves[page >> LeafBits].store(leaf, std::memory_order_release);
        }
        leaf->state[page & (PagesPerLeaf - 1)] = Armed;
    }
    // Whole range, whatever the states say: a remap the hooks have not reported yet leaves an
    // armed page writable, and protecting it again is free of side effects.
    protect(first << PageBits, (end - first) << PageBits, false);
    return stamp;
}

bool GuestWriteWatch::WrittenSince(VAddr address, u64 size, u64 stamp) const {
    if (epoch.load(std::memory_order_acquire) == stamp) {
        return false;
    }
    const u64 end = (address + size + PageBytes - 1) >> PageBits;
    for (u64 page = address >> PageBits; page < end; ++page) {
        const Leaf* leaf = LeafOf(page);
        if (!leaf ||
            leaf->written[page & (PagesPerLeaf - 1)].load(std::memory_order_relaxed) > stamp) {
            return true;
        }
    }
    return false;
}

GuestWriteWatch::FaultResult GuestWriteWatch::OnWriteFault(VAddr address) {
    const u64 faulted = address >> PageBits;
    if (faulted >= PageCount) {
        return FaultResult::NotOurs;
    }
    Leaf* leaf = LeafOf(faulted);
    if (!leaf) {
        return FaultResult::NotOurs;
    }
    std::scoped_lock lock{mutex};
    const u32 faulted_index = u32(faulted & (PagesPerLeaf - 1));
    const u8 state = leaf->state[faulted_index];
    if (state == Unknown) {
        return FaultResult::NotOurs;
    }
    faults.fetch_add(1, std::memory_order_relaxed);

    if (state != Armed) {
        protect(faulted << PageBits, PageBytes, true);
        return FaultResult::Retry;
    }
    const u32 written = NextEpoch();
    const u64 block = faulted & ~u64(PagesPerFault - 1);
    ReleaseRun run{*this};
    for (u64 page = block; page < block + PagesPerFault; ++page) {
        const u32 index = u32(page & (PagesPerLeaf - 1));
        if (leaf->state[index] != Armed) {
            run.Flush();
            continue;
        }
        Disarm(*leaf, index);
        if (page == faulted) {
            RecordWrite(*leaf, index, written);
        } else {
            RecordNeighbor(*leaf, index, written);
        }
        run.Add(page);
    }
    return FaultResult::Released;
}

template <typename Visit>
void GuestWriteWatch::ForEachKnownPage(VAddr address, u64 size, Visit&& visit) {
    if (address >= AddressLimit) {
        return;
    }
    const u64 end = std::min<u64>(
        (std::min(size, AddressLimit - address) + address + PageBytes - 1) >> PageBits, PageCount);
    for (u64 page = address >> PageBits; page < end;) {
        Leaf* leaf = LeafOf(page);
        const u64 leaf_end = (page | (PagesPerLeaf - 1)) + 1;
        if (leaf) {
            for (u64 at = page; at < std::min(leaf_end, end); ++at) {
                const u32 index = u32(at & (PagesPerLeaf - 1));
                if (leaf->state[index] != Unknown) {
                    visit(*leaf, index, at);
                }
            }
        }
        page = leaf_end;
    }
}

void GuestWriteWatch::BeforeHostWrite(VAddr address, u64 size) {
    std::scoped_lock lock{mutex};
    u32 written = 0;
    ReleaseRun run{*this};
    ForEachKnownPage(address, size, [&](Leaf& leaf, u32 index, u64 page) {
        written = written != 0 ? written : NextEpoch();
        if (leaf.state[index] == Armed) {
            Disarm(leaf, index);
            run.Add(page);
        } else {
            run.Flush();
        }
        RecordWrite(leaf, index, written);
    });
}

void GuestWriteWatch::NoteWritten(VAddr address, u64 size) {
    std::scoped_lock lock{mutex};
    u32 written = 0;
    ForEachKnownPage(address, size, [&](Leaf& leaf, u32 index, u64) {
        written = written != 0 ? written : NextEpoch();
        if (leaf.state[index] == Armed) {
            Disarm(leaf, index);
        }
        RecordWrite(leaf, index, written);
    });
}

// The hook can run before the unmap or after a remap, so the pages may still be mapped: they are
// left writable and kept from arming, or a live page would be read-only with nobody to release it.
void GuestWriteWatch::Forget(VAddr address, u64 size) {
    std::scoped_lock lock{mutex};
    const u32 now = frame.load(std::memory_order_relaxed);
    u32 written = 0;
    ReleaseRun run{*this};
    ForEachKnownPage(address, size, [&](Leaf& leaf, u32 index, u64 page) {
        written = written != 0 ? written : NextEpoch();
        if (leaf.state[index] == Armed) {
            Disarm(leaf, index);
            run.Add(page);
        } else {
            run.Flush();
        }
        leaf.state[index] = Unknown;
        leaf.written[index].store(written, std::memory_order_relaxed);
        leaf.strikes[index] = 0;
        leaf.last_write_frame[index] = 0;
        leaf.cool_until[index] = now + ForgottenCooldownFrames;
    });
}

void GuestWriteWatch::ReleaseAll() {
    std::scoped_lock lock{mutex};
    u32 written = 0;
    ReleaseRun run{*this};
    for (u32 slot = 0; slot < LeafCount; ++slot) {
        Leaf* leaf = leaves[slot].load(std::memory_order_relaxed);
        if (!leaf) {
            continue;
        }
        for (u32 index = 0; index < PagesPerLeaf; ++index) {
            if (leaf->state[index] != Armed) {
                continue;
            }
            written = written != 0 ? written : NextEpoch();
            Disarm(*leaf, index);
            leaf->written[index].store(written, std::memory_order_relaxed);
            run.Add((u64(slot) << LeafBits) | index);
        }
    }
}

} // namespace OpenGL
