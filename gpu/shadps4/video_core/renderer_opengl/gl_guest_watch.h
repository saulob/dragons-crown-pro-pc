// SPDX-License-Identifier: GPL-2.0-or-later
// Tells whether guest memory changed without reading it: pages are write protected, and the fault
// of the first write marks them. Knows nothing of what lives in those pages.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include "common/types.h"

namespace OpenGL {

/// The whole of a small range, three windows of a large one: reused many times per frame.
u64 HashGuestRange(const u8* data, u64 size);

/// False once one thread has retried a page too often with nothing changing: the fault is then a
/// real one, and must reach the crash report instead of spinning forever.
bool RetryFault(VAddr address);
void ResetFaultRetries();

class GuestWriteWatch {
public:
    /// Restricts a page aligned range to reads, or gives writes back.
    using ProtectFunction = void (*)(VAddr address, u64 size, bool writable);

    enum class FaultResult {
        NotOurs,  ///< No page there was ever armed.
        Released, ///< The page was armed and is writable now.
        Retry,    ///< Nothing to release: another thread did it, or the fault is not about us.
    };

    /// Addresses at or past this cannot be watched: the PS4's GPU sees 40 bits.
    static constexpr u64 AddressLimit = 1ull << 40;
    /// Long enough for a file read that touched a page to finish before the page is protected
    /// again: the kernel's copy fails on a read-only page, and no fault reports it.
    static constexpr u32 DefaultCooldownFrames = 16;

    explicit GuestWriteWatch(ProtectFunction protect,
                             u32 cooldown_frames = DefaultCooldownFrames);
    /// Armed pages are given back: none outlives the watch.
    ~GuestWriteWatch();

    void AdvanceFrame();

    /// Protects the whole range and returns the stamp WrittenSince takes, or nothing while a page
    /// in it was written too recently: the caller checks by content instead. Read guest memory
    /// only after this: a write made before is in what is read, one made after is reported.
    std::optional<u64> TryArm(VAddr address, u64 size);

    /// Lock free; true also for a page never armed.
    [[nodiscard]] bool WrittenSince(VAddr address, u64 size, u64 stamp) const;

    /// Any thread, in the fault handler.
    FaultResult OnWriteFault(VAddr address);

    /// Before the GPU thread writes the range: the same record as a fault, without one.
    void BeforeHostWrite(VAddr address, u64 size);

    /// Any thread: the protection over a range went away by other means than a fault.
    void NoteWritten(VAddr address, u64 size);

    /// Any thread: the range is being unmapped.
    void Forget(VAddr address, u64 size);

    /// Shutdown: gives every armed page back. Faults stay safe for as long as the object lives.
    void ReleaseAll();

    [[nodiscard]] u64 Faults() const {
        return faults.load(std::memory_order_relaxed);
    }

private:
    static constexpr u32 PageBits = 12;
    static constexpr u64 PageBytes = 1ull << PageBits;
    static constexpr u32 LeafBits = 12;
    static constexpr u64 PageCount = AddressLimit >> PageBits;
    static constexpr u32 PagesPerLeaf = 1u << LeafBits;
    static constexpr u32 LeafCount = u32(PageCount >> LeafBits);
    /// A fault takes this many pages at once: sequential writers would otherwise fault per page.
    static constexpr u32 PagesPerFault = 16;
    static constexpr u8 MaxStrikes = 6;
    static constexpr u32 ForgottenCooldownFrames = 64;

    enum PageState : u8 { Unknown, Writable, Armed };

    struct Leaf {
        std::array<std::atomic<u32>, PagesPerLeaf> written{};
        std::array<u32, PagesPerLeaf> last_write_frame{};
        std::array<u32, PagesPerLeaf> cool_until{};
        std::array<u8, PagesPerLeaf> state{};
        std::array<u8, PagesPerLeaf> strikes{};
    };

    /// Gives back pages run by run; touching nothing else keeps lock hold times short.
    class ReleaseRun {
    public:
        explicit ReleaseRun(GuestWriteWatch& watch_) : watch{watch_} {}
        ~ReleaseRun() {
            Flush();
        }
        void Add(u64 page);
        void Flush();

    private:
        GuestWriteWatch& watch;
        u64 begin{};
        u64 end{};
    };

    [[nodiscard]] Leaf* LeafOf(u64 page) const {
        return leaves[page >> LeafBits].load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 Cooldown(u8 strikes) const {
        return cooldown_frames << strikes;
    }
    [[nodiscard]] u32 NextEpoch() {
        return epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    /// A page written again soon after its last write earns a longer cooldown.
    void RecordWrite(Leaf& leaf, u32 index, u32 written);
    /// Released with a written page but not itself seen written: it only waits out the base.
    void RecordNeighbor(Leaf& leaf, u32 index, u32 written);
    void Disarm(Leaf& leaf, u32 index);
    template <typename Visit>
    void ForEachKnownPage(VAddr address, u64 size, Visit&& visit);

    ProtectFunction protect;
    u32 cooldown_frames;
    std::mutex mutex;
    std::atomic<u32> epoch{1};
    std::atomic<u32> frame{1};
    std::atomic<u64> faults{0};
    std::unique_ptr<std::atomic<Leaf*>[]> leaves;
};

} // namespace OpenGL
