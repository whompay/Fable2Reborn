/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

#if REX_ARCH_AMD64
#include <xmmintrin.h>
#endif  // REX_ARCH_AMD64

namespace rex::memory {

// Round 8 (2026-09-16, ReXGlue-general): minimal software-prefetch helper,
// ported in spirit from Xenia Canary's xe::swcache::Prefetch<Level2>
// (src/xenia/base/memory.h), trimmed to just the one hint tag this file
// actually uses (BeginPrefetchedRead below). Prefetch instructions are
// hints only: an address that turns out to be wrong or already cached is
// simply a wasted, side-effect-free instruction -- they cannot fault,
// cannot change observable state, and cannot alter correctness even if
// misused, which is why this is safe to add despite touching new ground.
// See native/performance-investigation-phase1.md, Optimization Round 8.
namespace swcache {
inline void PrefetchL2(const void* addr) {
#if REX_ARCH_AMD64
  _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T1);
#else
  (void)addr;
#endif  // REX_ARCH_AMD64
}
}  // namespace swcache

// Use uint32_t instead of size_t to eliminate REX prefix on x86-64
// instructions in this hot class.
using ring_size_t = uint32_t;

class RingBuffer {
 public:
  RingBuffer(uint8_t* buffer, size_t capacity);

  uint8_t* buffer() const { return buffer_; }
  ring_size_t capacity() const { return capacity_; }
  bool empty() const { return read_offset_ == write_offset_; }

  ring_size_t read_offset() const { return read_offset_; }
  uintptr_t read_ptr() const { return uintptr_t(buffer_) + static_cast<uintptr_t>(read_offset_); }
  void set_read_offset(size_t offset) {
    read_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }
  ring_size_t read_count() const {
    // Round 7 (2026-09-16, ReXGlue-general): ported from Xenia Canary
    // (RingBuffer::read_count), whose comment reads "these branches are
    // unpredictable". Upstream's own file-level note calls this class
    // "CRITICAL to the performance of the entire emulator" and separately
    // measured ReadAndSwap/read_count as still costing real, measured CPU
    // time (0.74%/0.23%) even after their other ring-buffer fixes -- the
    // same class of hot path Rounds 1-6 targeted, just one level lower:
    // every ring read of every kind (not just register writes) funnels
    // through BeginRead -> read_count. Whether read_offset_ <= write_offset_
    // depends on the ring's wraparound state, which has no fixed bias, so
    // it isn't reliably predicted either way. Upstream's fix computes both
    // possible results unconditionally before the branch (letting the
    // compiler use a conditional move instead of a real, mispredictable
    // branch) and marks the non-wrapped case as the likely one.
    // Semantically identical to the original two-branch form: unsigned
    // subtraction/addition never invokes undefined behavior even when the
    // "wrong" branch's expression underflows/overflows, because that value
    // is simply never returned -- verified against upstream's own
    // asm-level equivalence claim ("i checked the asm, msvc does not
    // automatically do this"). See
    // native/performance-investigation-phase1.md, Optimization Round 7.
    ring_size_t read_offs = read_offset_;
    ring_size_t write_offs = write_offset_;
    ring_size_t cap = capacity_;

    ring_size_t offset_delta = write_offs - read_offs;
    ring_size_t wrap_read_count = (cap - read_offs) + write_offs;

    if (read_offs <= write_offs) [[likely]] {
      return offset_delta;
    } else {
      return wrap_read_count;
    }
  }

  ring_size_t write_offset() const { return write_offset_; }
  uintptr_t write_ptr() const { return uintptr_t(buffer_) + static_cast<uintptr_t>(write_offset_); }
  void set_write_offset(size_t offset) {
    write_offset_ = static_cast<ring_size_t>(offset) % capacity_;
  }
  ring_size_t write_count() const {
    ring_size_t read_offs = read_offset_;
    ring_size_t write_offs = write_offset_;
    ring_size_t cap = capacity_;
    if (read_offs == write_offs) {
      return cap;
    } else if (write_offs < read_offs) {
      return read_offs - write_offs;
    } else {
      return (cap - write_offs) + read_offs;
    }
  }

  void AdvanceRead(size_t count);
  void AdvanceWrite(size_t count);

  struct ReadRange {
    const uint8_t* __restrict first;
    const uint8_t* __restrict second;
    ring_size_t first_length;
    ring_size_t second_length;
  };
  ReadRange BeginRead(size_t count);
  void EndRead(ReadRange read_range);

  // Round 8 (2026-09-16, ReXGlue-general): ported from Xenia Canary
  // (RingBuffer::BeginPrefetchedRead, src/xenia/base/ring_buffer.h). Upstream
  // calls this once per primary/indirect command buffer -- i.e. once per
  // frame, not per-packet -- right before the ExecutePacket loop that
  // processes every PM4 packet in that buffer (their comment: "prefetch the
  // wraparound range[;] it likely is already in L3 cache, but in a zen
  // system it may be another chiplet's L3"). This directly touches
  // ExecutePrimaryBuffer, independently confirmed as this project's own
  // 2nd-largest self-time contributor in the Round 3 Tracy capture (behind
  // only IssueDraw), so a stall hidden here is a stall hidden on exactly the
  // function this whole investigation has been trying to speed up.
  // BeginRead() is pure/side-effect-free (only EndRead mutates
  // read_offset_, confirmed by reading both), so calling it here and
  // discarding the result -- purely to obtain the addresses to prefetch --
  // changes nothing about the ring's read cursor or any subsequent real
  // read. Deliberately does not prefetch the first (contiguous, about to be
  // read sequentially anyway) range -- only the second, wrapped-around
  // range that a hardware prefetcher can't follow across the jump back to
  // the start of the buffer -- matching upstream's own reasoning exactly.
  // See native/performance-investigation-phase1.md, Optimization Round 8.
  void BeginPrefetchedRead(size_t count) {
    ReadRange range = BeginRead(count);
    if (range.second) {
      ring_size_t num_lines = (range.second_length + 63) / 64;
      for (ring_size_t i = 0; i < num_lines; ++i) {
        swcache::PrefetchL2(range.second + (size_t(i) * 64));
      }
    }
  }

  size_t Read(uint8_t* buffer, size_t count);
  template <typename T>
  size_t Read(T* buffer, size_t count) {
    return Read(reinterpret_cast<uint8_t*>(buffer), count);
  }

  template <typename T>
  T Read() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<uint8_t*>(&imm), sizeof(T));
    assert_true(read == sizeof(T));
    return imm;
  }

  template <typename T>
  T ReadAndSwap() {
    static_assert(std::is_fundamental<T>::value, "Immediate read only supports basic types!");

    T imm;
    size_t read = Read(reinterpret_cast<uint8_t*>(&imm), sizeof(T));
    assert_true(read == sizeof(T));
    imm = rex::byte_swap(imm);
    return imm;
  }

  size_t Write(const uint8_t* buffer, size_t count);
  template <typename T>
  size_t Write(const T* buffer, size_t count) {
    return Write(reinterpret_cast<const uint8_t*>(buffer), count);
  }

  template <typename T>
  size_t Write(T& data) {
    return Write(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
  }

 private:
  uint8_t* __restrict buffer_ = nullptr;
  ring_size_t capacity_ = 0;
  ring_size_t read_offset_ = 0;
  ring_size_t write_offset_ = 0;
};

// Fast path for GPU command processor - the hottest path.
// Direct load bypasses generic Read() + memcpy.
// Uses == (not >=) for wrap check: capacity must be a multiple of 4,
// so next_read_offset can only land exactly at capacity, never past it.
template <>
inline uint32_t RingBuffer::ReadAndSwap<uint32_t>() {
  assert_true(capacity_ >= 4);
  ring_size_t read_offs = read_offset_;
  ring_size_t next_read_offs = read_offs + 4;
  if (next_read_offs == capacity_) [[unlikely]] {
    next_read_offs = 0;
  }
  read_offset_ = next_read_offs;
  uint32_t ring_value = *(uint32_t*)(buffer_ + read_offs);
  return rex::byte_swap(ring_value);
}

}  // namespace rex::memory
