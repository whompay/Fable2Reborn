#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <rex/logging.h>

// Read-only PPC diagnostics. No process-lifetime cap: changes remain visible
// after the front end. Fixed per-thread storage avoids locks and allocations.
namespace fable2::page_diag {
inline void Trace(const char* site, uint32_t self, uint32_t lr, uint32_t arg5,
                  uint32_t mode, uint32_t state, uint32_t entering,
                  uint32_t complete) {
  struct Sample {
    const char* site = nullptr;
    std::array<uint32_t, 7> values{};
    std::chrono::steady_clock::time_point time{};
  };
  thread_local std::array<Sample, 64> samples{};
  const std::array<uint32_t, 7> values{self, lr, arg5, mode, state, entering, complete};
  const auto now = std::chrono::steady_clock::now();
  Sample* selected = nullptr;
  for (auto& sample : samples) {
    if (sample.site == site && sample.values[0] == self) {
      selected = &sample;
      break;
    }
    if (!sample.site && !selected) selected = &sample;
  }
  if (!selected) selected = &samples[(self >> 4) % samples.size()];
  if (selected->site == site && selected->values == values &&
      now - selected->time < std::chrono::seconds(2)) return;
  *selected = {site, values, now};
  REXLOG_INFO("[PAGE063-V1] {} self=0x{:08X} lr=0x{:08X} arg5={} mode84={} state88={} entering154={} complete153={}",
              site, self, lr, arg5, mode, state, entering, complete);
}
}  // namespace fable2::page_diag
