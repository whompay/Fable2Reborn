#include "../../tools/rexglue-source/src/graphics/d3d12/fable2_morph_readback.h"
using rex::graphics::d3d12::IsFable2MorphResolve;
static_assert(IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12704000, 6, 0));
static_assert(IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12704000, 6, 3));
static_assert(!IsFable2MorphResolve(0, 8, 3, 0x12704000, 6, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 4, 3, 0x12704000, 6, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 8, 6, 0x12704000, 6, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12700000, 6, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12704000, 0, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12704000, 1, 0));
static_assert(!IsFable2MorphResolve(0x4D5307F1, 8, 3, 0x12704000, 6, 4));
