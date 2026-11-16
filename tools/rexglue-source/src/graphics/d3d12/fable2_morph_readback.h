#pragma once
#include <cstdint>

namespace rex::graphics::d3d12 {
// GOTY/Platinum morph staging resolve, identified by Femtofork for Fable II:
// src/xenia/f2/hacks.h and gpu/pm4_command_processor_implement.h.
// Include format/depth checks to exclude other copies sharing the staging area.
// Note: xenos::ColorFormat::k_8_8_8_8 (RGBA8) has enum value 6.
constexpr bool IsFable2MorphResolve(uint32_t title, uint32_t primitive,
                                   uint32_t count, uint32_t destination,
                                   uint32_t format, uint32_t source) {
  return title == 0x4D5307F1 && primitive == 8 && count == 3 &&
         destination == 0x12704000 && format == 6 && source < 4;
}
}  // namespace rex::graphics::d3d12
