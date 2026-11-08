// Focused tests of the persistent pipeline cache rules (DCP-5MPDDU): blob sealing, the cache
// identity, the warm-up mode, failure handling, the build gate and atomic writes. No Vulkan.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "common/serdes.h"
#include "video_core/pipeline_cache_policy.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

using namespace PipelineCachePolicy;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

const StoreIdentity kRx580{0x1002, 0x6fdf, 1, 7, 7, 5};
const GpuIdentity kRx580Driver{0x1002, 0x6fdf, 1, 0x800117, 0x403104};

Bytes ReadAll(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

Bytes Sealed(Bytes bytes) {
    Seal(bytes);
    return bytes;
}

bool WriteBlob(const fs::path& file, Bytes payload) {
    Seal(payload);
    return WriteFileAtomically(file, payload);
}

std::optional<Bytes> LoadBlob(const fs::path& file) {
    auto bytes = ReadAll(file);
    if (!Unseal(bytes)) {
        return std::nullopt;
    }
    return bytes;
}

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path{fs::temp_directory_path() / name} {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

// A: first run fills an empty cache; the next start reads every entry back.
void TestFirstAndSecondRun() {
    TempDir dir{"dcp-pipeline-cache-a"};
    CHECK(CheckStore({}, kRx580) == StoreVerdict::Missing);
    CHECK(WriteBlob(dir.path / "identity.bin", EncodeStoreIdentity(kRx580)));
    for (int i = 0; i < 81; ++i) {
        CHECK(WriteBlob(dir.path / ("g_" + std::to_string(i) + ".key"), Bytes(64 + i, std::uint8_t(i))));
    }
    const auto identity = LoadBlob(dir.path / "identity.bin");
    CHECK(identity && CheckStore(*identity, kRx580) == StoreVerdict::Reuse);
    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        if (entry.path().extension() == ".key") {
            const auto blob = LoadBlob(entry.path());
            loaded += blob && blob->size() >= 64;
        }
        CHECK(!IsTempFile(entry.path()));
    }
    CHECK(loaded == 81);
}

// B, C: same GPU reuses; another vendor, device, driver family or cache version rebuilds.
void TestStoreIdentity() {
    const auto stored = EncodeStoreIdentity(kRx580);
    CHECK(CheckStore(stored, kRx580) == StoreVerdict::Reuse);
    auto other = kRx580;
    other.vendor_id = 0x10de;
    CHECK(CheckStore(stored, other) == StoreVerdict::Incompatible);
    other = kRx580;
    other.device_id = 0x67df;
    CHECK(CheckStore(stored, other) == StoreVerdict::Incompatible);
    other = kRx580;
    other.driver_id = 2;
    CHECK(CheckStore(stored, other) == StoreVerdict::Incompatible);
    other = kRx580;
    other.pipeline_key_version = 6;
    CHECK(CheckStore(stored, other) == StoreVerdict::Incompatible);
    auto broken = stored;
    broken.pop_back();
    CHECK(CheckStore(broken, kRx580) == StoreVerdict::Corrupt);
    CHECK(CheckStore(Bytes(stored.size(), 0), kRx580) == StoreVerdict::Corrupt);
}

// D: a driver update keeps the SPIR-V store and drops only the driver-dependent warm-up mode.
void TestDriverDependentData() {
    const auto policy = EncodeWarmupPolicy({kRx580Driver, 1});
    CHECK(ChooseWarmupMode(policy, kRx580Driver) == WarmupMode::Serial);
    auto updated = kRx580Driver;
    updated.driver_version += 1;
    CHECK(ChooseWarmupMode(policy, updated) == WarmupMode::Parallel);
    CHECK(CheckStore(EncodeStoreIdentity(kRx580), kRx580) == StoreVerdict::Reuse);
    CHECK(ChooseWarmupMode({}, kRx580Driver) == WarmupMode::Parallel);
    CHECK(ChooseWarmupMode(EncodeWarmupPolicy({kRx580Driver, 0}), kRx580Driver) ==
          WarmupMode::Parallel);
    CHECK(ChooseWarmupMode(EncodeStoreIdentity(kRx580), kRx580Driver) == WarmupMode::Parallel);
}

// E: truncated, empty, zero-filled, bit-flipped and pre-seal blobs are all rejected.
void TestDamagedBlobs() {
    const Bytes payload(300, 0x5a);
    const auto good = Sealed(payload);
    auto copy = good;
    CHECK(Unseal(copy) && copy == payload);
    for (std::size_t cut : {std::size_t(1), std::size_t(8), SealSize, good.size() - 1}) {
        Bytes truncated(good.begin(), good.end() - std::ptrdiff_t(cut));
        CHECK(!Unseal(truncated));
    }
    Bytes empty;
    CHECK(!Unseal(empty));
    Bytes zeros(good.size(), 0);
    CHECK(!Unseal(zeros));
    auto flipped = good;
    flipped[17] ^= 1;
    CHECK(!Unseal(flipped));
    auto old_format = payload;
    CHECK(!Unseal(old_format) && old_format == payload);
    auto empty_payload = Sealed({});
    CHECK(Unseal(empty_payload) && empty_payload.empty());
}

// F: an entry that does not match its format throws CorruptData, which callers skip.
void TestInvalidEntry() {
    Serialization::Archive ar{Bytes{1, 2, 3}};
    Serialization::Reader reader{ar};
    bool threw = false;
    try {
        std::uint64_t value{};
        reader.Read(value);
    } catch (const Serialization::CorruptData&) {
        threw = true;
    }
    CHECK(threw);

    Serialization::Archive counted;
    Serialization::Writer writer{counted};
    writer.Write(std::size_t{1u << 30});
    Serialization::Archive huge{counted.TakeOff()};
    Serialization::Reader huge_reader{huge};
    threw = false;
    try {
        std::vector<std::uint32_t> values;
        huge_reader.Read(values);
    } catch (const Serialization::CorruptData&) {
        threw = true;
    }
    CHECK(threw);
}

template <typename F>
bool ThrowsCorruptData(F&& read) {
    try {
        read();
    } catch (const Serialization::CorruptData&) {
        return true;
    }
    return false;
}

Bytes Serialized(const auto&... values) {
    Serialization::Archive ar;
    Serialization::Writer writer{ar};
    (writer.Write(values), ...);
    return ar.TakeOff();
}

struct FixedList {
    using iterator = std::uint32_t*;
    iterator begin() { return items; }
    iterator end() { return items + count; }
    std::size_t size() const { return count; }
    static constexpr std::size_t max_size() { return 4; }
    void emplace_back() { items[count++] = 0; }
    std::uint32_t& back() { return items[count - 1]; }
    std::uint32_t items[4]{};
    std::size_t count{};
};

// F2: lengths and sizes read from a sealed entry are proven against the bytes left before anything
// is allocated or read; the offset arithmetic cannot wrap.
void TestMalformedLengths() {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    for (const std::size_t length : {std::size_t(9), std::size_t(1) << 31, std::size_t(1) << 62, kMax - 7, kMax}) {
        Serialization::Archive ar{Serialized(length, std::uint32_t{0x41424344})};
        Serialization::Reader reader{ar};
        std::string text;
        CHECK(ThrowsCorruptData([&] { reader.Read(text); }));
        CHECK(text.empty() && text.capacity() < 64);
    }

    Serialization::Archive at_end{Bytes(8, 1)};
    Serialization::Reader end_reader{at_end};
    std::uint64_t eight{};
    end_reader.Read(eight);
    CHECK(at_end.Remaining() == 0 && at_end.IsEoS());
    std::uint8_t one{};
    CHECK(ThrowsCorruptData([&] { end_reader.Read(one); }));
    CHECK(ThrowsCorruptData([&] { at_end.Require(1); }));
    CHECK(ThrowsCorruptData([&] { at_end.Advance(1); }));
    at_end.Require(0);
    at_end.Advance(0);

    Serialization::Archive partial{Bytes(16, 2)};
    Serialization::Reader partial_reader{partial};
    partial_reader.Read(one);
    char sink[32];
    CHECK(ThrowsCorruptData([&] { partial_reader.Read(sink, 16); }));
    CHECK(ThrowsCorruptData([&] { partial_reader.Read(sink, kMax); }));
    CHECK(ThrowsCorruptData([&] { partial_reader.Read(sink, kMax - 1); }));
    CHECK(partial.Remaining() == 15);
    partial_reader.Read(sink, 15);
    CHECK(partial.Remaining() == 0);

    // A declared walker code size larger than the bytes left is refused before its pointer is used.
    Serialization::Archive walker{Bytes(28, 3)};
    Serialization::Reader walker_reader{walker};
    std::uint32_t head{};
    walker_reader.Read(head);
    CHECK(walker.Remaining() == 24);
    CHECK(ThrowsCorruptData([&] { walker.Require(25); }));
    CHECK(ThrowsCorruptData([&] { walker.Require(kMax); }));
    walker.Require(24);

    // A container count the remaining bytes cannot hold, or a fixed-capacity container cannot take.
    for (const std::size_t count : {std::size_t(9), std::size_t(1) << 40, kMax}) {
        Serialization::Archive ar{Serialized(count, std::uint32_t{1}, std::uint32_t{2})};
        Serialization::Reader reader{ar};
        std::vector<std::uint32_t> values;
        CHECK(ThrowsCorruptData([&] { reader.Read(values); }));
        CHECK(values.empty() && values.capacity() < 64);
    }
    Serialization::Archive short_list{Serialized(std::size_t{3}, std::uint32_t{1}, std::uint32_t{2})};
    Serialization::Reader short_reader{short_list};
    std::vector<std::uint32_t> short_values;
    CHECK(ThrowsCorruptData([&] { short_reader.Read(short_values); }));
    Serialization::Archive full{Serialized(std::size_t{5}, std::uint32_t{1}, std::uint32_t{1}, std::uint32_t{1},
                                           std::uint32_t{1}, std::uint32_t{1})};
    Serialization::Reader full_reader{full};
    FixedList list;
    CHECK(ThrowsCorruptData([&] { full_reader.Read(list); }));
    CHECK(list.count == 0);
}

// F3: a sealed entry cut short inside its payload is rejected by the reader; a valid one is read back.
void TestSealedEntries() {
    const std::vector<std::uint32_t> numbers{10, 20, 30};
    const std::vector<std::string> names{"alpha", "", "gamma"};
    auto valid = Serialized(std::string{"bits"}, numbers, names, std::uint64_t{99});
    auto sealed = Sealed(valid);
    CHECK(Unseal(sealed));
    Serialization::Archive ar{std::move(sealed)};
    Serialization::Reader reader{ar};
    std::string text;
    std::vector<std::uint32_t> read_numbers;
    std::vector<std::string> read_names;
    std::uint64_t tail{};
    reader.Read(text);
    reader.Read(read_numbers);
    reader.Read(read_names);
    reader.Read(tail);
    CHECK(text == "bits" && read_numbers == numbers && read_names == names && tail == 99);
    CHECK(ar.IsEoS());

    FixedList four;
    Serialization::Archive fixed{Serialized(std::size_t{4}, std::uint32_t{1}, std::uint32_t{2}, std::uint32_t{3},
                                            std::uint32_t{4})};
    Serialization::Reader fixed_reader{fixed};
    fixed_reader.Read(four);
    CHECK(four.count == 4);

    for (const std::size_t cut : {std::size_t(1), std::size_t(5), valid.size() - 1}) {
        auto truncated_payload = Bytes(valid.begin(), valid.end() - std::ptrdiff_t(cut));
        auto resealed = Sealed(truncated_payload);
        CHECK(Unseal(resealed));
        Serialization::Archive cut_ar{std::move(resealed)};
        Serialization::Reader cut_reader{cut_ar};
        CHECK(ThrowsCorruptData([&] {
            std::string a;
            std::vector<std::uint32_t> b;
            std::vector<std::string> c;
            std::uint64_t d{};
            cut_reader.Read(a);
            cut_reader.Read(b);
            cut_reader.Read(c);
            cut_reader.Read(d);
        }));
    }
    auto torn = Sealed(valid);
    torn.resize(torn.size() - 1);
    CHECK(!Unseal(torn));
}

// G: a preloaded pipeline is dropped; the game's own gets exactly one paused retry.
void TestFailurePolicy() {
    static_assert(OnPipelineFailure(PipelineOrigin::Preload, false) ==
                  FailureAction::DropCachedEntry);
    static_assert(OnPipelineFailure(PipelineOrigin::Preload, true) ==
                  FailureAction::DropCachedEntry);
    static_assert(OnPipelineFailure(PipelineOrigin::Game, false) ==
                  FailureAction::RetryWithWarmUpPaused);
    static_assert(OnPipelineFailure(PipelineOrigin::Game, true) == FailureAction::Fatal);
    int attempts = 0;
    for (bool retried = false;; retried = true) {
        ++attempts;
        if (OnPipelineFailure(PipelineOrigin::Game, retried) == FailureAction::Fatal) {
            break;
        }
    }
    CHECK(attempts == 2);
}

// H: six workers build in parallel; once serial is enabled and the parallel builds drained, no
// build runs without the lock and never two at a time.
void TestBuildGate() {
    BuildGate gate;
    std::atomic<bool> stop{false}, drained{false};
    std::atomic<int> serial_active{0}, parallel_after_drain{0}, overlap{0}, builds{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&] {
            while (!stop) {
                const BuildGate::Slot slot{gate};
                if (slot.Parallel()) {
                    if (drained) {
                        ++parallel_after_drain;
                    }
                } else if (++serial_active > 1) {
                    ++overlap;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                if (!slot.Parallel()) {
                    --serial_active;
                }
                ++builds;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!gate.Serial());
    CHECK(gate.EnableSerial());
    CHECK(!gate.EnableSerial());
    gate.WaitForParallelBuilds();
    drained = true;
    const int before = builds;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    stop = true;
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(parallel_after_drain == 0);
    CHECK(overlap == 0);
    CHECK(builds > before);
}

// I: a write cut short leaves the previous complete file; the stray .tmp is never a blob.
void TestInterruptedWrite() {
    TempDir dir{"dcp-pipeline-cache-i"};
    const auto file = dir.path / "g_0x0000000000000001.key";
    CHECK(WriteBlob(file, Bytes(100, 1)));
    {
        auto partial = Sealed(Bytes(100, 2));
        partial.resize(partial.size() / 2);
        std::ofstream stream(fs::path(file) += ".tmp", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(partial.data()), std::streamsize(partial.size()));
    }
    const auto previous = LoadBlob(file);
    CHECK(previous && *previous == Bytes(100, 1));
    const fs::path temp = fs::path(file) += ".tmp";
    CHECK(IsTempFile(temp) && temp.extension() != ".key");
    CHECK(!LoadBlob(temp));
    CHECK(WriteBlob(file, Bytes(120, 3)));
    const auto replaced = LoadBlob(file);
    CHECK(replaced && *replaced == Bytes(120, 3));
    CHECK(!fs::exists(temp));
}

} // namespace

int main() {
    TestFirstAndSecondRun();
    TestStoreIdentity();
    TestDriverDependentData();
    TestDamagedBlobs();
    TestInvalidEntry();
    TestMalformedLengths();
    TestSealedEntries();
    TestFailurePolicy();
    TestBuildGate();
    TestInterruptedWrite();
    if (g_failures) {
        std::printf("%d pipeline cache test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("pipeline cache tests passed\n");
    return 0;
}
