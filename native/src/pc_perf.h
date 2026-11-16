#pragma once

// PERF-1006 (2026-10-06): process-wide performance setup. See pc_perf.cpp.
namespace fable2::perf {
void Startup();
void Shutdown();
}  // namespace fable2::perf
