// DCP_INPUT_LATENCY_TRACE=1: input and presentation delays, one line each every 5 seconds.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace DcpLatencyTrace {
inline bool Requested(const char* env) {
    return env && env[0] == '1';
}
inline const bool enabled = Requested(std::getenv("DCP_INPUT_LATENCY_TRACE"));
/// SDL_GetTicksNS time of the newest gamepad button or key press the window thread saw.
inline std::atomic<std::uint64_t> press_ns{0};
} // namespace DcpLatencyTrace
