#include "appearance_loader.h"

#include <exception>

namespace dcp {

AppearancePrepared PrepareAppearance(const AppearanceRequest& request) {
    AppearancePrepared prepared;
    prepared.art = std::make_unique<CharacterArt>(request.sources);
    prepared.ok = prepared.art->Prepare(request.klass, request.color, request.look);
    if (!prepared.ok) prepared.error = prepared.art->error().empty() ? "the character art is unreadable" : prepared.art->error();
    return prepared;
}

AppearanceLoader::~AppearanceLoader() {
    if (worker_.joinable()) worker_.join();
}

uint64_t AppearanceLoader::Start(Job job) {
    if (worker_.joinable() || !job) return 0;
    done_.store(false);
    const uint64_t serial = ++serial_;
    worker_ = std::thread([this, job = std::move(job)] {
        AppearancePrepared prepared;
        try {
            prepared = job();
        } catch (const std::exception& error) {
            prepared = AppearancePrepared{nullptr, false, error.what()};
        } catch (...) {
            prepared = AppearancePrepared{nullptr, false, "unexpected error"};
        }
        result_ = std::move(prepared);
        done_.store(true, std::memory_order_release);
    });
    return serial;
}

std::optional<std::pair<uint64_t, AppearancePrepared>> AppearanceLoader::Collect() {
    if (!worker_.joinable() || !done_.load(std::memory_order_acquire)) return std::nullopt;
    worker_.join();
    return std::make_pair(serial_, std::move(result_));
}

} // namespace dcp
