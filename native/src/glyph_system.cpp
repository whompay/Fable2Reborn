#include "glyph_system.h"
#include "input_bindings.h"
#include "cursor_lock.h"
#include "raw_mouse.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/xenos.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "anark_render_bindings.h"
#include "gui_bank_x360_layout.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <bit>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <xinput.h>
#endif

namespace fable2::glyph {

namespace {

// ---------------------------------------------------------------------------
// VFS bank redirection (KBM/controller glyph swap)
//
// Supersedes the runtime RAM-injection approach below (LocateBnkBaseInMemory /
// OnResourceResolved's memory patch / WriteGuestPixels): that approach relied
// on gui_textures.bnk being resident in guest RAM as one monolithic block,
// which it never is -- it is a Mode="memory"-less, on-demand streamed VFS
// Bank (see extracted/data/startup.vfsconfig), so it is loaded from disk in
// pieces as the guest actually needs each texture, not scanned/found once in
// RAM. The kEnableLegacyMemoryPatch flag below disables that path outright
// (kept in the file for reference only, per investigation notes); this
// redirect is the active mechanism instead.
//
// Two pre-verified, pre-repacked sibling banks already exist on disk next to
// the original (see extracted/data/art/gui/):
//   gui_textures_x360.bnk  - byte-identical copy of the original bank
//   gui_textures_kbm.bnk   - same bank with only the 11 controller-glyph
//                            texture regions replaced by KBM key art
// Neither the original gui_textures.bnk nor gui_texture_headers.bnk is ever
// modified by this code.
//
// Redirection happens at the VFS symbolic-link layer (rex::filesystem::
// VirtualFileSystem::RegisterSymbolicLink/UnregisterSymbolicLink), the same
// mechanism ReXGlue itself uses to alias "game:" and "d:" to the mounted
// host-path device (see Runtime::SetupVfs in the ReXGlue runtime). We
// register a link for the bank's *fully-resolved* NT device path rather than
// a "game:"-prefixed path: VirtualFileSystem::ResolveSymbolicLink chains
// through symlinks by unordered prefix match, so keying on "game:\..." could
// race against ReXGlue's own "game:" -> "\Device\Harddisk0\Partition1" link
// depending on unordered_map iteration order. Keying on the fully-resolved
// path means our link can only ever be considered *after* "game:"/"d:" has
// already been substituted away, which is deterministic.
//
// IMPORTANT -- CONFIRMED LIMITATION, file-level symlink superseded:
// A first version of this redirect keyed the VFS symlink on the *file* path
// (".../gui/gui_textures.bnk"). It registered successfully every run, but
// had zero effect in-game. A full REX_LOG_LEVEL=trace capture proved why:
// Fable II's bank loader (log thread name "Permanent Bank Mounter") opens
// the "art/gui" *directory* once (an absolute-path open, which IS symlink-
// aware -- it shows up as "VFS resolved '...art\\gui' via symlink ..." in
// the trace), then opens each bank file inside it *relative to that
// directory handle*. NtCreateFile_entry (xboxkrnl_io.cpp) passes that
// directory's Entry as `root_entry` for such opens, and
// VirtualFileSystem::OpenFile, when `root_entry` is non-null, resolves via
// `root_entry->ResolvePath()` -- the base Entry::ResolvePath(), a plain
// GetChild() tree walk with **zero symlink awareness** (entry.cpp). VFS
// symlinks are only ever consulted by the *other* branch (root_entry ==
// nullptr). Across a full boot-to-shutdown trace, gui_textures.bnk never
// once produced a "VFS resolved" trace line -- unlike every other absolute
// path opened in the same run -- confirming its open goes through the
// symlink-blind relative path exclusively. No file-level symlink, keyed any
// way, can ever intercept it.
//
// Fix: redirect one level up, at the *directory* the guest actually
// resolves through the symlink-aware absolute-path open. This is the same
// symlink mechanism, just applied at the layer the trace proves is reached.
// `native/setup_glyph_dirs.ps1` (run once, no rebuild needed) prepares two
// sibling directories next to the real art/gui/ via NTFS hardlinks (zero
// extra disk beyond directory entries): _gui_kbm/ and _gui_x360/, each
// containing every file gui/ has, with gui_textures.bnk itself hardlinked
// to gui_textures_kbm.bnk / gui_textures_x360.bnk respectively. The real
// gui/gui_textures.bnk is never touched. Redirecting "art\\gui" -> "art\\
// _gui_kbm" (or "..._x360") makes the Bank Mounter's directory open resolve
// into the mirrored directory, so its subsequent relative child lookups for
// gui_textures.bnk (and everything else in that folder) walk the mirrored
// Entry tree instead -- no root_entry-bypass involved, because the redirect
// now happens before the symlink-blind part of resolution is ever reached.
//
// TARGET NAMING IS LOAD-BEARING (2026-09-11): the target basenames must NOT
// start with the literal substring "gui", or the game deadlocks on boot the
// instant anything is resolved through this symlink. Root cause:
// rex::filesystem::VirtualFileSystem::ResolveSymbolicLink() (vendor source:
// tools/rexglue-source/src/filesystem/virtual_file_system.cpp) matches a
// registered symlink's source against the resolved-so-far path with a raw
// utf8_starts_with_case() -- no path-separator awareness -- inside a
// `while (true)` loop that keeps re-matching against ALL registered
// symlinks (including the one it just used) until nothing matches anymore.
// With source ".../art/gui" and a target of ".../art/gui_kbm", the
// rewritten path after one substitution is ".../art/gui_kbm/<rest>", which
// *itself* still starts with ".../art/gui" (because "gui_kbm" starts with
// "gui") -- so the same symlink matches again, appending another "_kbm"
// forever. That loop runs while holding
// rex::thread::global_critical_region's process-wide recursive_mutex
// (documented in mutex.h as "must be the first lock acquired" and "extremely
// fast, no IO"), so it doesn't just hang one thread -- it starves every
// other thread in the process of that lock, freezing the entire game with
// zero further log output. This was misdiagnosed for a while as being about
// the *content* of the repacked KBM bank, because it only ever manifested
// the first time something actually resolved a path through this symlink
// during real boot -- which never happened before the redirect was moved to
// OnPostLoadXexImage() (every earlier hook registered too late to affect the
// Bank Mounter thread's one-time, cached directory mount). Confirmed via
// fable2_023.log (gui_kbm target) and fable2_024.log (gui_x360 target)
// freezing identically at the same point, ruling out bank content and
// isolating it to this prefix-collision bug instead. See
// glyph-vfs-redirect-status.md for the full investigation. The leading
// underscore in _gui_kbm/_gui_x360 makes the very next character after
// ".../art/" diverge from "gui", so the target can never again match the
// source as a prefix -- do not rename these back to gui_kbm/gui_x360 (or to
// anything else starting with "gui").
constexpr char kGuiDirGuestPath[] = "\\Device\\Harddisk0\\Partition1\\data\\art\\gui";
constexpr char kGuiDirKbmTarget[] = "\\Device\\Harddisk0\\Partition1\\data\\art\\_gui_kbm";
constexpr char kGuiDirX360Target[] = "\\Device\\Harddisk0\\Partition1\\data\\art\\_gui_x360";

// false = keep gui/ pointed at the x360 mirror (i.e. no redirect needed
// initially); tracked so we only touch the VFS symlink table when the
// desired target actually changes.
std::atomic<bool> g_bank_redirect_registered{false};
std::atomic<bool> g_bank_redirect_is_kbm{false};

// Points the "art/gui" directory at gui_kbm/ or gui_x360/ depending on the
// active input device. Safe to call frequently: it is a no-op unless the
// desired target actually changed, and it is safe to call before the VFS is
// set up (kernel_state()/file_system() are null-checked).
// 2026-11-15 Live glyph switching: the Bank Mounter honours art/gui only once,
// at boot, so following the device here can never change already-mounted
// banks -- it could only produce a KBM/x360 headers-vs-textures mismatch if a
// switch landed between the two mounts. The redirect is therefore pinned to
// the KBM mirror; live switching is done in memory instead (see the
// "2026-11-15 Live glyph switching" block at the end of this file).
constexpr bool kPinBankRedirectToKbm = true;

void UpdateBankRedirect(bool want_kbm) {
  if (kPinBankRedirectToKbm) want_kbm = true;
  if (g_bank_redirect_registered.load(std::memory_order_acquire) &&
      g_bank_redirect_is_kbm.load(std::memory_order_acquire) == want_kbm) {
    // DIAGNOSTIC (2026-09-12): this is the "already correct, no-op" path --
    // taken on the overwhelming majority of calls once the redirect first
    // registers, since OnResourceResolved calls UpdateBankRedirect() on
    // *every* gui resource resolve. Log the first few no-op confirmations so
    // a capture can show the redirect was already in the desired (KBM) state
    // by the time a given menu/title resolve happened, without flooding the
    // log on every single frame thereafter. This directly supports checking
    // whether "menu glyph wrong" correlates with a genuine race (redirect
    // not yet applied when the menu's bank read happened) or not (redirect
    // already correct well before, so the wrongness must be a cache
    // downstream of the file read, not a timing race at the VFS layer).
    static std::atomic<int> s_noop_confirm_count{0};
    int c = s_noop_confirm_count.fetch_add(1, std::memory_order_relaxed);
    if (c < 10) {
      REXLOG_INFO("[GlyphSystem] [VFS] [DIAG] redirect already correct (want_kbm={}) confirm #{}", want_kbm, c + 1);
    }
    return;
  }

  auto* kstate = rex::system::kernel_state();
  if (!kstate) return;
  auto* vfs = kstate->file_system();
  if (!vfs) return;

  // RegisterSymbolicLink() silently no-ops if the source path is already
  // registered (unordered_map::insert does not overwrite), so the existing
  // link must be removed before the new target can take effect.
  vfs->UnregisterSymbolicLink(kGuiDirGuestPath);

  const char* target = want_kbm ? kGuiDirKbmTarget : kGuiDirX360Target;
  if (vfs->RegisterSymbolicLink(kGuiDirGuestPath, target)) {
    g_bank_redirect_registered.store(true, std::memory_order_release);
    g_bank_redirect_is_kbm.store(want_kbm, std::memory_order_release);
    REXLOG_INFO("[GlyphSystem] [VFS] art/gui directory redirected -> {}", target);
  } else {
    g_bank_redirect_registered.store(false, std::memory_order_release);
    REXLOG_WARN("[GlyphSystem] [VFS] Failed to register directory redirect to {}", target);
  }
}

// The runtime RAM-injection path below (kAButtonHeaderSigBE/LE signature
// scan, LocateBnkBaseInMemory, the pixel-buffer-locating half of
// OnResourceResolved, WriteGuestPixels) is retained for reference only and
// is intentionally disabled: the VFS redirect above supersedes it. Flip this
// back to true only to resume debugging that approach specifically.
constexpr bool kEnableLegacyMemoryPatch = false;

// EXPERIMENTAL, DISPROVEN (2026-09-12, fable2_045.log round enabled it;
// fable2_047.log round conclusively disproved it and disabled it below --
// see glyph-vfs-redirect-status.md for the full round-4 writeup). Kept in
// the file for reference only, matching the established pattern for
// kEnableLegacyMemoryPatch above. Completely separate from, and independent
// of, kEnableLegacyMemoryPatch.
//
// Original rationale (fable2_044.log/045.log): the per-button header at
// bnk_base+kButtonBnkDefs[...].bnk_offset (the ONLY buffer the legacy path
// above ever tried to write through) is static, byte-identical, pure
// metadata for every capture -- word[2] is always 0, and no 6-word window
// anywhere in its 88 bytes decodes as a valid xe_gpu_texture_fetch_t. So
// this experiment targeted `resource_guest_ptr` instead -- the OTHER,
// already-present argument to OnResourceResolved(), which passed
// IsValidGuestReadPtr and whose raw dumped bytes looked high-entropy and
// image-like.
//
// DISPROVEN this round (fable2_047.log): the guarded write fired, was
// verified via readback (write_verified=true, hash_actual_after ==
// hash_expected_after, 16384 bytes written to 0x448CC960), and had ZERO
// visual effect anywhere -- not title, not world, not menu. Combined with
// this round's independent discovery (see GameNameHash32() below and
// sub_822CA580 in fable2_recomp.66.cpp), we now know exactly why:
// `resource_guest_ptr` is NOT a pixel-buffer pointer at all. It is the
// 32-bit return value of the game's own path-name hash function
// (GameNameHash32-style FNV-1-order hash over the resource's full resolved
// path string). Numerically verified: GameNameHash32("art\\gui\\
// controller\\icon_button_a.tex") == 0x448CC960, an EXACT match to the
// resource_guest_ptr this experiment wrote 16KB of pixel data into. Writing
// pixel-shaped bytes at a guest address that merely happens to equal a name
// hash, in a large committed heap, can succeed mechanically (the address
// passes IsValidGuestReadPtr/WritePtr and the bytes really do get written
// and read back correctly) while being completely disconnected from any
// texture the GPU ever samples -- which is exactly the "write verified, no
// visual change" result fable2_047.log reported. This experiment is
// conclusively DISPROVEN, not merely inconclusive; do not re-enable it
// without a new theory for what resource_guest_ptr's memory actually is,
// since we now have a fully-traced, verified explanation for what it is NOT.
//
// TryExperimentalAWriteForA() below is kept intact and still fully
// functional if flipped back to true (nothing about the guard chain itself
// was wrong -- it did exactly what it was designed to do, and its own
// verification logging is what proved the write landed). The
// backup-and-restore path in UpdateAllSlots() is itself gated on
// g_exp_a_written, so leaving this false is safe (nothing left dangling).
constexpr bool kEnableExperimentalAWrite = false;

inline uint8_t GuestLoadU8(const uint8_t* base, uint32_t addr) {
  return *(base + addr);
}

inline uint16_t GuestLoadU16(const uint8_t* base, uint32_t addr) {
  return std::byteswap(*reinterpret_cast<const uint16_t*>(base + addr));
}

inline uint32_t GuestLoadU32(const uint8_t* base, uint32_t addr) {
  return std::byteswap(*reinterpret_cast<const uint32_t*>(base + addr));
}

inline void GuestStoreU8(uint8_t* base, uint32_t addr, uint8_t value) {
  *(base + addr) = value;
}

inline void GuestStoreU16(uint8_t* base, uint32_t addr, uint16_t value) {
  *reinterpret_cast<uint16_t*>(base + addr) = std::byteswap(value);
}

inline void GuestStoreU32(uint8_t* base, uint32_t addr, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + addr) = std::byteswap(value);
}

enum class ButtonType {
  kA,
  kB,
  kX,
  kY,
  kLT,
  kRT,
  kLB,
  kRB,
  kStart,
  kBack,
  kDPad,
  kLStick,
  kRStick,
  kUnknown,
};

// Human-readable name for log lines (2026-09-13, fable2_048.log round).
const char* ButtonTypeName(ButtonType t) {
  switch (t) {
    case ButtonType::kA: return "A";
    case ButtonType::kB: return "B";
    case ButtonType::kX: return "X";
    case ButtonType::kY: return "Y";
    case ButtonType::kLT: return "LT";
    case ButtonType::kRT: return "RT";
    case ButtonType::kLB: return "LB";
    case ButtonType::kRB: return "RB";
    case ButtonType::kStart: return "Start";
    case ButtonType::kBack: return "Back";
    case ButtonType::kDPad: return "DPad";
    case ButtonType::kLStick: return "LStick";
    case ButtonType::kRStick: return "RStick";
    default: return "Unknown";
  }
}

struct Slot {
  ButtonType type = ButtonType::kUnknown;
  std::string name;
  uint32_t resource_guest_ptr = 0;
  uint32_t texture_guest_ptr = 0;
  uint32_t base_address = 0; // Guest physical/virtual address of pixel buffer
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bpp = 4; // Bytes per pixel: 4 for RGBA, 1 for Alpha
  bool tiled = false;
  size_t buffer_size = 0;
  std::vector<uint8_t> original_xbox_pixels;
  std::string last_pc_glyph;
};

std::atomic<InputDevice> g_active_device{InputDevice::kController};

// Timestamp (milliseconds, std::chrono::steady_clock epoch) of the most
// recent REAL relative mouse motion event (event.dx()/dy() != 0), written by
// OnMouseEvent (UI thread) and read by OnInputPolled (guest thread) -- see
// the 2026-09-15 "D-pad/arrow-key vs. right-stick overlap" fix in
// OnInputPolled for why this exists: MnkInputDriver (closed-source SDK
// component) drives the right thumbstick from BOTH real mouse motion
// (mouse-look) AND, evidently, the arrow keys as a built-in keyboard camera
// fallback -- confirmed by user report, not by reading MnkInputDriver's
// source (none is shipped). There is no cvar/binding for this in our own
// input_bindings.h system (kLeftStick/kRightStick are deliberately excluded
// from GameAction -- see that file's header comment), and no exposed way to
// disable it inside MnkInputDriver itself. This timestamp is the only signal
// available to tell "the right stick moved because of a real mouse motion"
// apart from "the right stick moved because an arrow key is held" without
// touching MnkInputDriver at all: if no real mouse motion happened very
// recently, any right-stick deflection while a D-pad-bound key is held is
// attributed to the arrow-key fallback and zeroed in guest memory only (see
// OnInputPolled). Plain std::atomic<int64_t>, not a mutex -- single writer,
// high-frequency reader, and a torn/stale read here only affects a 100ms
// suppression window, never correctness of mouse-look itself (mouse-look is
// never written by us; we only ever zero the stick, never invent a value).
std::atomic<int64_t> g_last_real_mouse_motion_ms{0};

std::mutex g_glyph_mutex;
std::vector<Slot> g_slots;
uint8_t* g_ppc_base = nullptr;
bool g_initialized = false;

// EXPERIMENTAL (2026-09-12, fable2_045.log round): guarded, isolated,
// reversible direct-write experiment targeting ONLY the A button's
// `resource_guest_ptr` -- see kEnableExperimentalAWrite below and the
// TryExperimentalAWriteForA() function just above OnResourceResolved() for
// full rationale. These globals hold the one-shot "did we already write"
// latch and the original-bytes backup so the write can be undone the same
// way UpdateAllSlots() already undoes the (disabled) legacy per-slot writes
// when the active input device switches back to a controller. Guarded by
// g_glyph_mutex (the same lock UpdateAllSlots()/OnResourceResolved's legacy
// path use) -- never touched without holding it.
bool g_exp_a_written = false;
uint32_t g_exp_a_guest_addr = 0;
std::vector<uint8_t> g_exp_a_backup;
std::string g_exp_a_glyph_name;

// Cache of raw KBM glyph pixel data: key -> raw byte buffer
std::unordered_map<std::string, std::vector<uint8_t>> g_glyph_cache;

// Standard Xenos 2D tiling function
int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

void TilePixels(const uint8_t* linear_src, uint8_t* tiled_dst, uint32_t width, uint32_t height, uint32_t bpp) {
  uint32_t bpb_log2 = (bpp == 4) ? 2 : 0;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int32_t tiled_offset = GetTiledOffset2D(static_cast<int32_t>(x), static_cast<int32_t>(y), width, bpb_log2);
      uint32_t linear_offset = (y * width + x) * bpp;
      for (uint32_t b = 0; b < bpp; ++b) {
        tiled_dst[tiled_offset + b] = linear_src[linear_offset + b];
      }
    }
  }
}

// Simple FNV-1a 32-bit hash -- used only as a cheap before/after/expected
// content fingerprint for [EXP-A-WRITE] logging (2026-09-12, fable2_046.log
// round), so a log alone can prove a guest-memory write actually landed
// without dumping the whole 16KB buffer.
uint32_t Fnv1aHash32(const uint8_t* data, size_t size) {
  uint32_t hash = 0x811C9DC5u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x01000193u;
  }
  return hash;
}

// THE GAME'S OWN hash algorithm (2026-09-12, fable2_047.log round --
// reverse-engineered from sub_822CA580, native/generated/default/
// fable2_recomp.66.cpp lines 2159-2308, codegen.partition.json function
// index 66, guest address 0x822CA580). This is UNRELATED to Fnv1aHash32
// above (our own diagnostic hash, used only for before/after write-buffer
// verification) -- this is the actual algorithm Fable II itself uses to
// turn a resource's full resolved path string into the 32-bit value this
// file has been calling `resource_guest_ptr` since round 1 of this
// investigation.
//
// Traced call chain that computes and carries this value: sub_82817748
// (fable2_recomp.69.cpp) -> sub_82BFD780 (fable2_recomp.84.cpp, builds the
// formatted path string) -> sub_822C88A8 (fable2_recomp.95.cpp, 4-line
// tail-call thunk) -> sub_822F2088 (fable2_recomp.281.cpp, a memoized/
// lazy-cache accessor keyed on an object field) -> sub_822CA580
// (fable2_recomp.66.cpp, THIS function) -- which hashes the string and
// caches the result. Confirmed by exact numeric match, verified
// independently in Python:
//   GameNameHash32("art\\gui\\controller\\icon_button_a.tex") == 0x448CC960
// which is EXACTLY the resource_guest_ptr value every capture in this
// investigation has logged for icon_button_a.tex.
//
// Algorithm, decompiled from the PPC loop at loc_822CA650 in
// sub_822CA580: FNV-1 ORDER (multiply-then-xor: hash = (hash * prime) XOR
// byte), NOT canonical FNV-1a (xor-then-multiply) -- confirmed via the
// decompiled constants `lis r3,-32484; ori r3,r3,40389` (0x811C9DC5, the
// standard FNV offset basis) and `lis r9,256; ori r9,r9,403` (0x01000193,
// the standard FNV prime). The game iterates UTF-16 code units and uses
// only the low byte of each (`clrlwi r8,r10,24` == code_unit & 0xFF),
// stopping at (and NOT including) the null terminator. There is also a
// length-normalization pre-step for names >= 40 characters (via
// sub_822CA680/sub_822F5150/sub_822F6480/sub_822F20F0) that this C++
// re-implementation does not reproduce -- confirmed not to matter for any
// icon_button_*.tex / NxN/*.tex name, all of which are well under 40
// characters and go straight through the plain hash loop at loc_822CA628.
uint32_t GameNameHash32(const char* ascii_name) {
  uint32_t hash = 0x811C9DC5u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(ascii_name); *p; ++p) {
    hash = (hash * 0x01000193u) ^ static_cast<uint32_t>(*p);
  }
  return hash;
}

// Convert string to lower case
std::string ToLower(std::string_view str) {
  std::string res;
  res.reserve(str.size());
  for (char c : str) {
    res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return res;
}

// Identify button type from resource path
ButtonType IdentifyButton(std::string_view name_view) {
  std::string name = ToLower(name_view);
  
  if (name.find("icon_button_a") != std::string::npos) return ButtonType::kA;
  if (name.find("icon_button_b") != std::string::npos) return ButtonType::kB;
  if (name.find("icon_button_x") != std::string::npos) return ButtonType::kX;
  if (name.find("icon_button_y") != std::string::npos) return ButtonType::kY;
  if (name.find("lefttrigger") != std::string::npos || name.find("controller_lt") != std::string::npos) return ButtonType::kLT;
  if (name.find("righttrigger") != std::string::npos || name.find("controller_rt") != std::string::npos) return ButtonType::kRT;
  if (name.find("leftbutton") != std::string::npos || name.find("controller_lb") != std::string::npos) return ButtonType::kLB;
  if (name.find("rightbutton") != std::string::npos || name.find("controller_rb") != std::string::npos) return ButtonType::kRB;
  if (name.find("start") != std::string::npos) return ButtonType::kStart;
  if (name.find("back") != std::string::npos) return ButtonType::kBack;
  if (name.find("dpad") != std::string::npos) return ButtonType::kDPad;
  if (name.find("24x24\\left") != std::string::npos || name.find("24x24/left") != std::string::npos ||
      name.find("controller_left") != std::string::npos) return ButtonType::kLStick;
  if (name.find("24x24\\right") != std::string::npos || name.find("24x24/right") != std::string::npos ||
      name.find("controller_right") != std::string::npos) return ButtonType::kRStick;

  return ButtonType::kUnknown;
}

// EXPERIMENTAL-A-WRITE bugfix (2026-09-12, fable2_046.log round): resolve
// the project's actual root directory (the folder containing extracted/,
// native/, pc/, tools/) from the running executable's own location -- the
// EXACT same technique fable2_app.h's App::OnConfigurePaths() already uses
// (successfully, every real run) to locate `game_data_root`: walk up from
// GetModuleFileNameW()'s directory, up to 7 levels, looking for the
// extracted/default.xex marker file. LoadBinaryAsset() below anchors its
// search on this instead of purely CWD-relative candidate paths, because
// `fable2_046.log` proved those CWD-relative paths silently found nothing:
// EVERY one of the pc/assets/glyphs/*.bin files (space_64.bin included,
// 16384 bytes, confirmed present on disk with the correct size for a 64x64
// RGBA8 buffer) was never being found, which is exactly why
// GetGlyphPixels("space", 64, 4) always returned an empty buffer and
// TryExperimentalAWriteForA() always aborted at the "too small (0 <
// 16384)" check. The process's current working directory at runtime is
// evidently NOT the project root (e.g. launched via a shortcut/debugger
// with a different "start in" directory) -- exactly the class of bug
// `OnConfigurePaths()` already exists to work around for game_data_root/
// user_data_root/cache_root, just never applied to this file's own asset
// loader until now.
std::filesystem::path GetGameRootDir() {
  static std::filesystem::path s_game_root; // empty until resolved; cached after first call
  static bool s_resolved = false;
  if (s_resolved) return s_game_root;
  s_resolved = true;
#if defined(_WIN32)
  wchar_t executable[32768] = {};
  auto length = GetModuleFileNameW(nullptr, executable, 32768);
  if (length && length < 32768) {
    std::filesystem::path directory = std::filesystem::path(executable).parent_path();
    for (int level = 0; level < 7; ++level) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(directory / "extracted" / "default.xex", ec)) {
        s_game_root = directory;
        REXLOG_INFO("[GlyphSystem] [DIAG] GetGameRootDir() resolved project root via executable-path walk-up: '{}'",
                    s_game_root.string());
        break;
      }
      auto parent = directory.parent_path();
      if (parent == directory) break;
      directory = parent;
    }
  }
  if (s_game_root.empty()) {
    REXLOG_WARN("[GlyphSystem] [DIAG] GetGameRootDir() FAILED to find extracted/default.xex within 7 levels above the executable -- LoadBinaryAsset() will fall back to CWD-relative paths only");
  }
#endif
  return s_game_root;
}

// Load a binary file from several prospective search paths
std::vector<uint8_t> LoadBinaryAsset(const std::string& filename) {
  std::vector<std::string> tried_paths;

  auto try_read = [&](const std::filesystem::path& full_path) -> std::vector<uint8_t> {
    tried_paths.push_back(full_path.string());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full_path, ec)) return {};
    std::ifstream f(full_path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
  };

  // Primary: anchor to the actual project root, independent of CWD.
  std::filesystem::path game_root = GetGameRootDir();
  if (!game_root.empty()) {
    auto buf = try_read(game_root / "pc" / "assets" / "glyphs" / filename);
    if (!buf.empty()) {
      REXLOG_INFO("[GlyphSystem] [DIAG] LoadBinaryAsset('{}') resolved via game-root-anchored path ({} bytes): '{}'",
                  filename, buf.size(), tried_paths.back());
      return buf;
    }
  }

  // Fallback: legacy CWD-relative candidates -- these were the ONLY paths
  // tried before this round, and (per fable2_046.log) silently returned
  // empty every time the process's working directory wasn't already the
  // project root. Kept as a secondary attempt for portability/dev setups
  // where CWD does happen to be right, and so a future change to the
  // launch working directory doesn't reintroduce this exact failure mode
  // silently.
  const std::vector<std::string> search_dirs = {
    "pc/assets/glyphs/",
    "../pc/assets/glyphs/",
    "../../pc/assets/glyphs/",
    "assets/glyphs/",
  };
  for (const auto& dir : search_dirs) {
    auto buf = try_read(std::filesystem::path(dir) / filename);
    if (!buf.empty()) {
      REXLOG_INFO("[GlyphSystem] [DIAG] LoadBinaryAsset('{}') resolved via CWD-relative fallback path ({} bytes): '{}'",
                  filename, buf.size(), tried_paths.back());
      return buf;
    }
  }

  // Nothing found anywhere -- log every path attempted (capped per distinct
  // filename) so a future failure like fable2_046.log's is immediately
  // diagnosable from the log alone instead of silently returning empty.
  {
    static std::mutex s_load_fail_mutex;
    static std::unordered_map<std::string, int> s_load_fail_occurrences;
    std::lock_guard<std::mutex> lock(s_load_fail_mutex);
    int& fail_count = s_load_fail_occurrences[filename];
    if (fail_count < 5) {
      ++fail_count;
      std::string joined;
      for (size_t i = 0; i < tried_paths.size(); ++i) {
        if (i) joined += " | ";
        joined += tried_paths[i];
      }
      REXLOG_WARN("[GlyphSystem] [DIAG] LoadBinaryAsset('{}') FAILED -- tried: {}", filename, joined);
    }
  }
  return {};
}

// Dynamic PC binding resolver (2026-09-13 rewrite): game action -> current
// binding -> glyph, via input_bindings.h. Previously this function read
// "keybind_a"/"keybind_x"/etc. cvars that were never registered anywhere
// (rex::cvar::GetFlagByName on an unregistered name returns ""), so every
// call silently fell through to the hardcoded default string below -- the
// "mouse_active" branch above it read an equally-unregistered "mnk_mouse"
// cvar and was therefore also always true in practice. Both facts made this
// function look dynamic while actually being 100% hardcoded. input_bindings.h
// now REGISTERS real, hot-reloadable string cvars for every button handled
// here (kLStick/kRStick/kDPad excepted -- see that file's header comment),
// parsed through the SDK's own exact VirtualKey parser instead of a substring
// scan, so a real rebind (SetFlagByName, a config edit, or a future settings
// UI) changes the glyph glyph_system.cpp draws on the very next call --
// Initialize()'s existing RegisterChangeCallback wiring (unchanged) is what
// makes that happen without waiting for a device switch.
std::string ResolvePcGlyphForButton(ButtonType type) {
  auto resolve = [](GameAction action, const char* fallback) -> std::string {
    std::string glyph = GlyphNameForBinding(GetCurrentBinding(action));
    return glyph.empty() ? fallback : glyph;
  };

  switch (type) {
    // A is multi-bound to E/Space/Shift (E primary as of 2026-09-15 --
    // interact/roll/sprint) -- GetCurrentBinding(kA) already returns
    // whichever of those is FIRST in keybind_a's list that parses bound
    // (currently E), so this shows one glyph for all three contexts. KNOWN
    // AMBIGUITY, not fixable without new information: the game always
    // requests the same "icon_button_a" resource regardless of whether
    // it's currently showing an interact/roll/sprint prompt (see
    // IdentifyButton() below -- no separate name pattern exists for any of
    // the three), so we have no signal to pick Space for a roll prompt and
    // Shift for a sprint prompt distinctly. Reported, not guessed;
    // reordering keybind_a's cvar value changes which single glyph shows
    // here -- this is queried from the actual binding, not hardcoded.
    case ButtonType::kA: return resolve(GameAction::kA, "e");
    // X/Y (2026-09-14): no longer resolved from keybind_x/keybind_y (those
    // cvars are legacy -- see input_bindings.cpp). LMB/RMB are fixed,
    // non-cvar-backed physical inputs by explicit design (Fable III's own
    // scheme hardcodes LMB=primary/RMB=secondary, not something meant to be
    // independently rebindable this round) -- there is no binding to query
    // here, unlike every other case in this switch, so "lmb" is a genuine
    // constant, not a shortcut around an existing cvar. It's also
    // mode-invariant (same answer in Melee and Ranged), so no
    // GetCurrentCombatMode() read is needed. KNOWN GAP: melee-block (RMB,
    // hold X) and ranged-aim (RMB, hold Y) have no distinct glyph here, for
    // the same reason as kA above -- the game only ever requests
    // "icon_button_x"/"icon_button_y", never a separate name for its
    // block/aim vs. attack prompt, so we cannot show RMB for those contexts
    // without guessing. Reported in AI_HANDOFF.md rather than guessed.
    case ButtonType::kX: return "lmb";
    case ButtonType::kY: return "lmb";
    // B (2026-09-15): UNLIKE X/Y, B genuinely has a real, queryable cvar
    // now (keybind_b, default Escape) -- it was repurposed as the menu
    // Back/Cancel button rather than left legacy, so this resolves from it
    // like every ordinary action instead of hardcoding "lmb". This means
    // the B icon favors the Back/Cancel reading (the more universal Xbox
    // convention, and the default startup combat mode is Melee, not
    // Magic, so B's combat role is the less common case at launch) over
    // the Magic-mode LMB-attack reading, which has no distinct glyph here
    // for the same icon_button_b-name-ambiguity reason as kA/kX/kY above.
    case ButtonType::kB: return resolve(GameAction::kB, "esc");
    case ButtonType::kLT: return resolve(GameAction::kLeftTrigger, "q");
    // RT/LB (2026-09-14, still open 2026-09-15 by explicit instruction):
    // keybind_right_trigger/keybind_left_shoulder default to "" (unbound --
    // their previous keys, E and 1, were claimed by A and combat-mode-select
    // respectively; see input_bindings.cpp). Deliberately left unassigned
    // pending confirmation of their real Fable II functions, rather than
    // guessed at. Falling back to "" here lets GetGlyphPixels' own existing
    // not-found fallback take over (ultimately shows the "space" glyph)
    // instead of showing a specific key that isn't actually bound to this
    // button.
    case ButtonType::kRT: return resolve(GameAction::kRightTrigger, "");
    case ButtonType::kLB: return resolve(GameAction::kLeftShoulder, "");
    case ButtonType::kRB: return resolve(GameAction::kRightShoulder, "r");
    // Fallback matches keybind_start's new default (P, see
    // input_bindings.cpp) -- only used if GlyphNameForBinding() itself
    // returns "" (unbound cvar or an unrecognized override), not in the
    // normal case, since VK::kP already has a mapped "p" glyph.
    case ButtonType::kStart: return resolve(GameAction::kStart, "p");
    case ButtonType::kBack: return resolve(GameAction::kBack, "tab");
    // Analog composites, not a single rebindable key/mouse control -- see
    // input_bindings.h's header comment and AI_HANDOFF.md.
    case ButtonType::kLStick: return "wasd";
    case ButtonType::kRStick: return "mouse_look";
    // D-pad (2026-09-19): the game requests one generic "dpad" icon for
    // the whole control, not a separate one per direction, so -- same
    // ambiguity class as kA/kX/kY/kB above -- we can only show one glyph
    // for all four F1-F4 hotkeys, same as kLStick/kRStick below: a fixed
    // constant rather than a per-binding resolve(). Previously fell back
    // to "" (ultimately "space") because no D-pad art existed at all --
    // "dpad_arrows" now does (native/generate_new_kbm_glyphs.py's combined
    // 4-direction glyph, baked to pc/assets/glyphs/dpad_arrows_{32,64}.bin
    // and dpad_arrows_32_alpha.bin), so show that instead of blank space
    // whenever KBM is active. Individual arrow_up/down/left/right glyphs
    // were baked too, for if per-direction display becomes possible later.
    case ButtonType::kDPad: return "dpad_arrows";
    default:
      return "space";
  }
}

// Get pixel data for a glyph name and format
const std::vector<uint8_t>& GetGlyphPixels(const std::string& name, uint32_t width, uint32_t bpp) {
  std::string cache_key = name + "_" + std::to_string(width) + (bpp == 1 ? "_alpha" : "");
  auto it = g_glyph_cache.find(cache_key);
  if (it != g_glyph_cache.end()) {
    REXLOG_INFO("[GlyphSystem] [MENU-GLYPH-TRACE] GetGlyphPixels cache hit: name='{}' width={} bpp={} cache_key='{}'",
                name, width, bpp, cache_key);
    return it->second;
  }

  std::string filename;
  if (width == 64) {
    filename = name + "_64.bin";
  } else {
    filename = name + (bpp == 1 ? "_32_alpha.bin" : "_32.bin");
  }

  REXLOG_INFO("[GlyphSystem] [MENU-GLYPH-TRACE] GetGlyphPixels loading: name='{}' width={} bpp={} filename='{}'",
              name, width, bpp, filename);

  std::vector<uint8_t> data = LoadBinaryAsset(filename);
  if (data.empty()) {
    // Fallback: if specific keycap file wasn't found, try space or e
    std::string fallback = (bpp == 1) ? "space_32_alpha.bin" : (width == 64 ? "space_64.bin" : "space_32.bin");
    REXLOG_WARN("[GlyphSystem] [MENU-GLYPH-TRACE] GetGlyphPixels fallback: primary '{}' empty, trying fallback '{}'",
                filename, fallback);
    data = LoadBinaryAsset(fallback);
  }

  auto [inserted_it, _] = g_glyph_cache.emplace(cache_key, std::move(data));
  return inserted_it->second;
}

} // namespace

// Defined later in this file (VirtualQuery-based Windows memory validation).
// Forward-declared here, at this same fable2::glyph scope (NOT inside the
// anonymous namespace above), so OnInputPolled -- which is defined before it
// but is also a direct fable2::glyph member, not an anonymous-namespace one
// -- can guard its gameplay-input-override writes with it. A previous
// version of this declaration was mistakenly placed inside the anonymous
// namespace above; since anonymous-namespace members are implicitly visible
// from the enclosing namespace too, that created a second, distinct
// (internal-linkage, never-defined) IsValidGuestWritePtr alongside this
// function's real definition below, and every unqualified call site saw
// both -- an ambiguous-call compile error, not a linker error, which is why
// it surfaced at every call site rather than at the declaration itself.
bool IsValidGuestWritePtr(const uint8_t* base, uint32_t guest_addr, size_t size);

void Initialize() {
  std::lock_guard<std::mutex> lock(g_glyph_mutex);
  if (g_initialized) return;
  g_initialized = true;

  REXLOG_INFO("[GlyphSystem] Initializing centralized dynamic glyph system...");

  // One-time startup diagnostic dump of every binding cvar's raw value and
  // resolved key/mouse code (see input_bindings.h/.cpp). Purely additive
  // logging -- does not affect glyph behavior or input handling.
  LogCurrentBindings();

  // Register cvar change callbacks to dynamically update button glyphs on key rebind
  const std::vector<std::string> keybind_cvars = {
    "keybind_a", "keybind_b", "keybind_x", "keybind_y",
    "keybind_left_trigger", "keybind_right_trigger",
    "keybind_left_shoulder", "keybind_right_shoulder",
    "keybind_start", "keybind_back",
    "mnk_mouse", "mnk_mode",
  };

  for (const auto& cvar_name : keybind_cvars) {
    rex::cvar::RegisterChangeCallback(cvar_name, [](std::string_view name, std::string_view value) {
      if (name == "keybind_a") {
        REXLOG_INFO("[InteractGlyph] binding callback: keybind_a='{}', kbm={}", value, IsKbmActive());
      }
      if (IsKbmActive()) {
        UpdateAllSlots();
      }
    });
  }

  // Determine initial device: check if any physical controller is connected
  bool any_controller_connected = false;
#if defined(_WIN32)
  typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
  static PFN_XInputGetState pfnXInputGetState = []() -> PFN_XInputGetState {
    HMODULE hMod = LoadLibraryA("xinput1_4.dll");
    if (!hMod) hMod = LoadLibraryA("xinput1_3.dll");
    if (!hMod) hMod = LoadLibraryA("xinput9_1_0.dll");
    if (hMod) {
      return reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hMod, "XInputGetState"));
    }
    return nullptr;
  }();

  if (pfnXInputGetState) {
    for (DWORD i = 0; i < 4; ++i) {
      XINPUT_STATE st{};
      if (pfnXInputGetState(i, &st) == ERROR_SUCCESS) {
        any_controller_connected = true;
        break;
      }
    }
  }
#endif

  // Always default active device to Keyboard/Mouse at startup on PC;
  // only real button/stick/trigger input will switch to Controller.
  g_active_device.store(InputDevice::kKBM);
  REXLOG_INFO("[GlyphSystem] Startup active device initialized to Keyboard/Mouse (physical controller present: {})", any_controller_connected);

  // Best-effort: the VFS is almost certainly not set up yet this early
  // (Initialize() runs from OnPreSetup, before Runtime::SetupVfs()), so this
  // will typically no-op via the null checks in UpdateBankRedirect.
  //
  // CONFIRMED BY TRACE (2026-09-11): relying on OnResourceResolved() to
  // "catch up" later is too late in practice. A REX_LOG_LEVEL=trace capture
  // showed the engine's "Permanent Bank Mounter" thread resolves and opens
  // the art/gui directory (unredirected) several times within ~3 seconds of
  // Runtime::SetupVfs() registering "game:"/"d:" -- and never resolves that
  // directory again for the rest of the session. OnResourceResolved()'s
  // first real call, by contrast, didn't land until ~10 seconds into boot,
  // long after the bank was already mounted from the original location.
  // See RefreshBankRedirect() below, called from
  // Fable2App::OnPostLoadXexImage() (fable2_app.h) once VFS exists and well
  // before the Bank Mounter thread runs, closing that window instead of
  // hoping a later hook catches up. (An earlier attempt called this from
  // OnPostSetup() instead and caused a boot hang -- see fable2_app.h.)
  UpdateBankRedirect(IsKbmActive());
}

// Re-applies the bank redirect for the currently active input device. Same
// no-op-if-unchanged/null-checked semantics as the Initialize()/
// SetActiveInputDevice()/OnResourceResolved() call sites -- safe to call
// redundantly. Exists so Fable2App::OnPostLoadXexImage() (fable2_app.h) can
// retry the redirect right after Runtime::LoadXexImage() returns, rather
// than waiting for the first OnResourceResolved() callback, which trace
// evidence showed fires after the engine has already mounted the
// (unredirected) bank directory. See the comment at the end of Initialize()
// for the full story.
void RefreshBankRedirect() {
  UpdateBankRedirect(IsKbmActive());
  // 2026-11-15: VFS/kernel exist now -- start the live glyph worker early so
  // its one-time bank read happens during boot, not on the first switch.
  NotifyLiveGlyphDeviceChanged();
}

void SetActiveInputDevice(InputDevice device) {
  InputDevice old = g_active_device.exchange(device, std::memory_order_acq_rel);
  // Unlike UpdateAllSlots() (which only matters for the disabled legacy
  // memory-patch path), the VFS redirect is refreshed unconditionally rather
  // than gated on old != device: Initialize() sets the startup device with a
  // raw store that bypasses this function, so the first call here with the
  // already-current device would otherwise never sync the redirect.
  UpdateBankRedirect(device == InputDevice::kKBM);
  if (old != device) {
    REXLOG_INFO("[GlyphSystem] Switched active input device to: {}",
                device == InputDevice::kKBM ? "Keyboard/Mouse" : "Controller");
    // 2026-11-15: wake the live glyph worker (bank textures + Anark render
    // objects are re-bound in memory, see end of file).
    NotifyLiveGlyphDeviceChanged();
    UpdateAllSlots();
  }
}

InputDevice GetActiveInputDevice() {
  return g_active_device.load(std::memory_order_relaxed);
}

bool IsKbmActive() {
  return g_active_device.load(std::memory_order_relaxed) == InputDevice::kKBM;
}

void OnKeyEvent(const rex::ui::KeyEvent& event, bool is_down) {
  // 2026-11-15 live glyph switching: only a FRESH key press (up -> down
  // transition) counts as "the player used the keyboard". Key-ups and OS
  // auto-repeat of a key that is merely being held do not, so a key held
  // down while the player picks up the controller can't keep stealing the
  // active device back (no glyph flicker). Last device actually used wins.
  static std::atomic<bool> s_key_down[256];  // zero-initialised (static storage)
  const uint32_t vk_index = static_cast<uint32_t>(event.virtual_key()) & 0xFFu;
  const bool was_down = s_key_down[vk_index].exchange(is_down, std::memory_order_relaxed);
  if (is_down && !was_down) {
    if (!IsKbmActive()) {
      REXLOG_INFO("[GlyphSystem] [DIAG] Key press received -> requesting switch to Keyboard/Mouse");
    }
    SetActiveInputDevice(InputDevice::kKBM);
  }
  // Feed the gameplay-input override layer (input_bindings.h) so
  // OnInputPolled's ApplyBindingsToGamepadState sees the same key state
  // that drove this device-switch decision -- one source of truth for
  // both the displayed glyph and actual gameplay input.
  NotifyKeyState(event.virtual_key(), is_down);
}

void OnMouseEvent(const rex::ui::MouseEvent& event, MouseEventKind kind) {
  // 2026-11-15 live glyph switching, hysteresis: a button press or wheel tick
  // switches to KBM at once; plain movement only once it is REAL movement --
  // at least kMinMoveEvents move events adding up to kMinMovePixels within
  // kMoveWindowMs. A single warp (cursor_lock's SetCursorPos re-centre, a
  // window focus change) or sensor jitter therefore never flips glyphs.
  bool kbm_activity = (kind == MouseEventKind::kDown || kind == MouseEventKind::kWheel);
  if (kind == MouseEventKind::kMove && (event.dx() != 0 || event.dy() != 0)) {
    constexpr int64_t kMoveWindowMs = 200;
    constexpr int kMinMovePixels = 8;
    constexpr int kMinMoveEvents = 2;
    static int64_t s_move_window_start_ms = 0;
    static int s_move_accum = 0;
    static int s_move_events = 0;
    const int64_t move_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    if (move_now_ms - s_move_window_start_ms > kMoveWindowMs) {
      s_move_window_start_ms = move_now_ms;
      s_move_accum = 0;
      s_move_events = 0;
    }
    s_move_accum += std::abs(event.dx()) + std::abs(event.dy());
    ++s_move_events;
    if (s_move_accum >= kMinMovePixels && s_move_events >= kMinMoveEvents) kbm_activity = true;
  }
  if (kbm_activity) {
    if (!IsKbmActive()) {
      REXLOG_INFO("[GlyphSystem] [DIAG] Mouse event received (dx={}, dy={}, btn={}) -> requesting switch to Keyboard/Mouse",
                  event.dx(), event.dy(), static_cast<int>(event.button()));
    }
    SetActiveInputDevice(InputDevice::kKBM);
  }

  // 2026-09-15: record real relative-motion timestamps for OnInputPolled's
  // D-pad/arrow-key vs. right-stick suppression (see g_last_real_mouse_motion_ms's
  // own comment above its declaration). Deliberately keyed on dx()/dy() only
  // (not button state) -- this is specifically "was the mouse actually
  // moved", independent of the broader device-switch check above.
  if (event.dx() != 0 || event.dy() != 0) {
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    g_last_real_mouse_motion_ms.store(now_ms, std::memory_order_relaxed);
  }

  switch (kind) {
    case MouseEventKind::kDown:
      NotifyMouseButtonState(event.button(), true);
      break;
    case MouseEventKind::kUp:
      NotifyMouseButtonState(event.button(), false);
      break;
    case MouseEventKind::kWheel:
      NotifyMouseWheel(event.scroll_y());
      break;
    case MouseEventKind::kMove:
      break;
  }
}

void OnPollController() {
  // Handled automatically via OnInputPolled on each frame
}

void OnInputPolled(uint32_t user_index, uint32_t state_guest_ptr, uint32_t xam_status, uint8_t* base) {
  fable2::cursor_lock::RequestUpdate();  // 2026-11-15 cursor lock per-frame tick
  g_ppc_base = base;

  // 1. Query real physical Windows XInput controller
#if defined(_WIN32)
  typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);
  static PFN_XInputGetState pfnXInputGetState = []() -> PFN_XInputGetState {
    HMODULE hMod = LoadLibraryA("xinput1_4.dll");
    if (!hMod) hMod = LoadLibraryA("xinput1_3.dll");
    if (!hMod) hMod = LoadLibraryA("xinput9_1_0.dll");
    if (hMod) {
      return reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hMod, "XInputGetState"));
    }
    return nullptr;
  }();

  XINPUT_STATE host_state{};
  // PERF-1006 (2026-10-06): XInputGetState on an EMPTY slot is slow (it probes
  // for a device every call) and the game polls every slot every frame. Once a
  // slot reports "not connected", skip re-probing it for 1 s; a newly plugged
  // in pad is still picked up within a second. Connected slots poll as before.
  static std::atomic<int64_t> s_next_probe_ms[4] = {0, 0, 0, 0};
  DWORD host_ret = ERROR_DEVICE_NOT_CONNECTED;
  if (pfnXInputGetState) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (user_index >= 4 || now_ms >= s_next_probe_ms[user_index].load(std::memory_order_relaxed)) {
      host_ret = pfnXInputGetState(user_index, &host_state);
      if (user_index < 4) {
        s_next_probe_ms[user_index].store(host_ret == ERROR_SUCCESS ? 0 : now_ms + 1000,
                                          std::memory_order_relaxed);
      }
    }
  }
  bool host_connected = (host_ret == ERROR_SUCCESS);

  // 2026-11-15 DS4/DualSense/Switch/generic pads: these are not XInput devices,
  // so XInputGetState never sees them and the pad was treated as unplugged
  // (KBM stayed active and overwrote its buttons/right stick). Ask the
  // runtime's SDL driver for the real pad in connection order instead. Looked
  // up at run time: with an older rexruntime.dll this is simply absent.
  if (!host_connected && user_index < 4) {
    using PFN_SdlPad = int (*)(uint32_t, uint16_t*, uint8_t*, uint8_t*, int16_t*);
    static PFN_SdlPad pfnSdlPad = []() -> PFN_SdlPad {
      HMODULE rt = GetModuleHandleA("rexruntime.dll");
      auto fn = rt ? reinterpret_cast<PFN_SdlPad>(GetProcAddress(rt, "rex_sdl_get_physical_pad"))
                   : nullptr;
      REXLOG_INFO("[GlyphSystem] SDL physical-pad query {}", fn ? "available (DS4/DualSense/"
                  "Switch/generic pads count as controllers)" : "NOT available (old "
                  "rexruntime.dll -- run BUILD_RUNTIME.bat; only XInput pads detected)");
      return fn;
    }();
    uint16_t b = 0;
    uint8_t lt = 0, rt = 0;
    int16_t st[4] = {0, 0, 0, 0};
    if (pfnSdlPad && pfnSdlPad(user_index, &b, &lt, &rt, st)) {
      host_state = {};
      host_state.Gamepad.wButtons = b;
      host_state.Gamepad.bLeftTrigger = lt;
      host_state.Gamepad.bRightTrigger = rt;
      host_state.Gamepad.sThumbLX = st[0];
      host_state.Gamepad.sThumbLY = st[1];
      host_state.Gamepad.sThumbRX = st[2];
      host_state.Gamepad.sThumbRY = st[3];
      host_ret = ERROR_SUCCESS;
      host_connected = true;
    }
  }
#else
  bool host_connected = false;
  uint32_t host_ret = 1167;
#endif

  // Inspect guest XAM state if available
  uint16_t gwButtons = 0;
  uint8_t gbLeftTrigger = 0, gbRightTrigger = 0;
  int16_t gsThumbLX = 0, gsThumbLY = 0, gsThumbRX = 0, gsThumbRY = 0;
  if (xam_status == 0 && state_guest_ptr != 0 && base != nullptr) {
    gwButtons = GuestLoadU16(base, state_guest_ptr + 4);
    gbLeftTrigger = GuestLoadU8(base, state_guest_ptr + 6);
    gbRightTrigger = GuestLoadU8(base, state_guest_ptr + 7);
    gsThumbLX = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 8));
    gsThumbLY = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 10));
    gsThumbRX = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 12));
    gsThumbRY = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 14));
  }

  // TEMPORARY DIAGNOSTIC (2026-09-14, architecture-verification round only):
  // checks whether any of the ten candidate default physical inputs (Space,
  // LeftShift, LMB, RMB, Q, E, 1, R, Tab, Escape) causes
  // RexGlue/MnkInputDriver to set any bit our post-poll override does NOT
  // own -- D-pad, thumbstick clicks (LEFT_THUMB/RIGHT_THUMB), Guide, or
  // either thumbstick's axis values -- by logging the RAW state captured
  // just above (gwButtons/gsThumb*), which is BEFORE the override block
  // below runs, whenever that raw state changes while one of the ten test
  // inputs is currently held. Deliberately NOT gated on the current
  // keybind_* cvars: this checks the PHYSICAL inputs directly, independent
  // of whatever they're bound to right now, per this round's explicit
  // request to verify before changing any defaults. Purely read-only --
  // does not write anything, does not touch the override below in any way.
  // Remove once this investigation is done.
  // PERF-1006: the verification round this served is done; off.
  constexpr bool kEnablePreOverrideStateDiag = false;
  if (kEnablePreOverrideStateDiag && xam_status == 0 && state_guest_ptr != 0 && base != nullptr) {
    struct PhysicalTestInput {
      const char* name;
      KbmBinding binding;
    };
    static const PhysicalTestInput kPhysicalTestInputs[] = {
        {"Space", ParseBindingString("Space")}, {"LeftShift", ParseBindingString("Shift")},
        {"LMB", ParseBindingString("LMB")},      {"RMB", ParseBindingString("RMB")},
        {"Q", ParseBindingString("Q")},          {"E", ParseBindingString("E")},
        {"1", ParseBindingString("1")},          {"R", ParseBindingString("R")},
        {"Tab", ParseBindingString("Tab")},      {"Escape", ParseBindingString("Escape")},
    };

    std::string held_names;
    for (const auto& test_input : kPhysicalTestInputs) {
      if (IsPhysicalBindingDown(test_input.binding)) {
        if (!held_names.empty()) held_names += "+";
        held_names += test_input.name;
      }
    }

    static uint16_t s_diag_last_buttons = 0;
    static uint8_t s_diag_last_lt = 0, s_diag_last_rt = 0;
    static int16_t s_diag_last_lx = 0, s_diag_last_ly = 0, s_diag_last_rx = 0, s_diag_last_ry = 0;
    bool raw_state_changed = gwButtons != s_diag_last_buttons || gbLeftTrigger != s_diag_last_lt ||
                              gbRightTrigger != s_diag_last_rt || gsThumbLX != s_diag_last_lx ||
                              gsThumbLY != s_diag_last_ly || gsThumbRX != s_diag_last_rx ||
                              gsThumbRY != s_diag_last_ry;

    if (raw_state_changed && !held_names.empty()) {
      constexpr uint16_t kDpadBits = 0x000F;        // UP|DOWN|LEFT|RIGHT
      constexpr uint16_t kThumbClickBits = 0x00C0;  // LEFT_THUMB|RIGHT_THUMB
      constexpr uint16_t kGuideBit = 0x0400;
      constexpr uint16_t kUnownedBits = kDpadBits | kThumbClickBits | kGuideBit;
      uint16_t unowned_bits_set = gwButtons & kUnownedBits;

      REXLOG_INFO(
          "[InputBindings] [DIAG] [PreOverride] held={} raw_buttons=0x{:04X} "
          "dpad=0x{:X} thumb_click=0x{:X} guide=0x{:X} UNOWNED_BITS=0x{:X} "
          "lt={} rt={} thumbLX={} thumbLY={} thumbRX={} thumbRY={}",
          held_names, gwButtons, gwButtons & kDpadBits, gwButtons & kThumbClickBits,
          gwButtons & kGuideBit, unowned_bits_set, gbLeftTrigger, gbRightTrigger, gsThumbLX,
          gsThumbLY, gsThumbRX, gsThumbRY);
    }

    s_diag_last_buttons = gwButtons;
    s_diag_last_lt = gbLeftTrigger;
    s_diag_last_rt = gbRightTrigger;
    s_diag_last_lx = gsThumbLX;
    s_diag_last_ly = gsThumbLY;
    s_diag_last_rx = gsThumbRX;
    s_diag_last_ry = gsThumbRY;
  }

  // 2. Gameplay-input override: while KBM is the active input device,
  // overwrite the guest's button bits/triggers from the same
  // GameAction -> KbmBinding cvars the glyph resolver reads (input_bindings.h)
  // -- ApplyBindingsToGamepadState fully replaces exactly the 8 action-owned
  // bits (see its own comment for why), leaving DPad/thumbsticks/Guide
  // untouched so movement/camera stay on MnkInputDriver's own WASD/mouse-look
  // mapping. This runs BEFORE the `!host_connected` early-return below on
  // purpose: KBM-only play (the common case) always has host_connected ==
  // false, so gating this after that return would mean the override never
  // fires in the ordinary case. Guarded on IsKbmActive() so plugging in and
  // using a real controller is untouched -- MnkInputDriver's own synthetic
  // state is only overridden while KBM is actually the active device, and
  // this never runs at all when xam_status/state_guest_ptr/base aren't valid.
  if (IsKbmActive() && xam_status == 0 && state_guest_ptr != 0 && base != nullptr) {
    uint16_t new_buttons = gwButtons;
    uint8_t new_left_trigger = gbLeftTrigger;
    uint8_t new_right_trigger = gbRightTrigger;
    ApplyBindingsToGamepadState(new_buttons, new_left_trigger, new_right_trigger);
    // Tracks whether OUR override actually changed any owned button bit or
    // trigger byte SINCE THE LAST POLL WE OURSELVES WROTE, so we know
    // whether to bump the guest's packet_number below.
    //
    // 2026-09-15 fix (charged/held attacks never accumulated, e.g. holding
    // LMB for a charged melee swing): the previous version compared
    // new_buttons/new_left_trigger/new_right_trigger against
    // gwButtons/gbLeftTrigger/gbRightTrigger -- this poll's freshly re-read
    // guest state, i.e. MnkInputDriver's OWN baseline for this poll, BEFORE
    // our override ran. MnkInputDriver recomputes its synthetic pad state
    // from scratch every poll from WASD/mouse-look and has no notion of
    // LMB/RMB -> X/Y at all, so its baseline for an owned bit like X is 0 on
    // EVERY poll, not just the first one after a press. That made "changed"
    // true, and packet_number bump, on every single poll for as long as LMB
    // stayed held -- not just on the actual press/release edge. To any
    // guest/XInput consumer that treats "packet_number changed" as "this is
    // a fresh input state, re-evaluate from scratch" (a standard XInput
    // consumption pattern, and the same assumption a held-duration/charge
    // timer would need to accumulate across polls), that is
    // indistinguishable from the button being freshly pressed every single
    // frame -- the state never reads as "unchanged" long enough to
    // accumulate a hold. Comparing against what WE ourselves last wrote
    // (persisted across polls, per controller slot) instead restores real
    // edge semantics: packet_number now only bumps on an actual transition
    // in our own output (press, release, or a genuine change in one of the
    // passthrough bits MnkInputDriver still owns, e.g. a thumbstick click),
    // exactly mirroring how MnkInputDriver's own packet_number_ behaves for
    // state changes it produces itself.
    static uint16_t s_last_applied_buttons[4] = {0, 0, 0, 0};
    static uint8_t s_last_applied_left_trigger[4] = {0, 0, 0, 0};
    static uint8_t s_last_applied_right_trigger[4] = {0, 0, 0, 0};
    uint32_t slot = user_index < 4 ? user_index : 0;

    bool changed = (new_buttons != s_last_applied_buttons[slot]) ||
                   (new_left_trigger != s_last_applied_left_trigger[slot]) ||
                   (new_right_trigger != s_last_applied_right_trigger[slot]);

    if (new_buttons != gwButtons && IsValidGuestWritePtr(base, state_guest_ptr + 4, 2)) {
      GuestStoreU16(base, state_guest_ptr + 4, new_buttons);
      gwButtons = new_buttons;
    }
    if (new_left_trigger != gbLeftTrigger && IsValidGuestWritePtr(base, state_guest_ptr + 6, 1)) {
      GuestStoreU8(base, state_guest_ptr + 6, new_left_trigger);
      gbLeftTrigger = new_left_trigger;
    }
    if (new_right_trigger != gbRightTrigger && IsValidGuestWritePtr(base, state_guest_ptr + 7, 1)) {
      GuestStoreU8(base, state_guest_ptr + 7, new_right_trigger);
      gbRightTrigger = new_right_trigger;
    }
    // 2026-11-15: left stick from WASD ignoring modifiers, so movement keeps
    // working while Shift (sprint) is held -- the SDK's exact-modifier match
    // silences plain W/A/S/D whenever Shift is down.
    if (IsValidGuestWritePtr(base, state_guest_ptr + 8, 4)) {
      int16_t klx = 0, kly = 0;
      GetKeyboardMoveStick(klx, kly);
      if (klx != gsThumbLX || kly != gsThumbLY) {
        GuestStoreU16(base, state_guest_ptr + 8, static_cast<uint16_t>(klx));
        GuestStoreU16(base, state_guest_ptr + 10, static_cast<uint16_t>(kly));
        gsThumbLX = klx;
        gsThumbLY = kly;
      }
    }
    if (changed && IsValidGuestWritePtr(base, state_guest_ptr, 4)) {
      uint32_t packet_number = GuestLoadU32(base, state_guest_ptr);
      GuestStoreU32(base, state_guest_ptr, packet_number + 1);
      // TEMPORARY DIAGNOSTIC (2026-09-15, charge-attack round): logs every
      // actual packet_number bump this override causes. Before this round's
      // fix, holding LMB/RMB produced one of these lines PER POLL for as
      // long as the button stayed held (dozens/sec); after the fix, a single
      // press+hold+release should produce exactly two lines close together
      // (the press edge, then -- much later -- the release edge), with
      // nothing in between while held steady. Remove once confirmed in a
      // runtime log.
      REXLOG_INFO(
          "[InputBindings] [DIAG] [PacketBump] slot={} buttons=0x{:04X} lt={} rt={} "
          "packet_number {}->{}",
          slot, new_buttons, new_left_trigger, new_right_trigger, packet_number, packet_number + 1);
    }
    s_last_applied_buttons[slot] = new_buttons;
    s_last_applied_left_trigger[slot] = new_left_trigger;
    s_last_applied_right_trigger[slot] = new_right_trigger;
  }

  // 2026-09-15: suppress the D-pad/arrow-key -> right-stick overlap.
  //
  // User report: the arrow keys are bound to the D-pad (keybind_dpad_up/
  // down/left/right = Up/Down/Left/Right, for Fable III-style
  // context-sensitive shortcuts) but ALSO nudge the camera, because
  // MnkInputDriver (the closed-source SDK component that turns keyboard/
  // mouse into a synthetic Xbox controller -- see mnk_input_driver.h) drives
  // the right thumbstick from the arrow keys as a built-in keyboard-camera
  // fallback, on top of real mouse-look. Confirmed there is no cvar for
  // this: every keybind_*-style cvar string actually compiled into the
  // binary was enumerated and none covers the sticks (kLeftStick/
  // kRightStick are deliberately excluded from GameAction, see
  // input_bindings.h's file comment), and MnkInputDriver ships no source,
  // only a header -- so this cannot be fixed by config, and MnkInputDriver
  // itself cannot be edited.
  //
  // Smallest fix available: while a D-pad-bound key is held AND no real
  // mouse motion has happened very recently (g_last_real_mouse_motion_ms,
  // updated by OnMouseEvent), the right stick's guest-visible values are
  // assumed to be the arrow-key fallback rather than mouse-look, and are
  // zeroed directly in guest memory -- the same guest XINPUT_GAMEPAD
  // structure the button override above already writes into, at the
  // sThumbRX/sThumbRY offsets read into gsThumbRX/gsThumbRY near the top of
  // this function. This does NOT call into or alter MnkInputDriver in any
  // way -- it only corrects what the guest ends up seeing, exactly once per
  // poll, after MnkInputDriver has already written its own synthetic state.
  // Left stick (gsThumbLX/LY, WASD) is never touched. Gated on the exact
  // same IsKbmActive()/xam_status/state_guest_ptr/base guard as the button
  // override above, so a real controller's right stick -- and this whole
  // block -- is completely inert unless KBM is the active input device.
  if (IsKbmActive() && xam_status == 0 && state_guest_ptr != 0 && base != nullptr) {
    bool dpad_key_held = IsActionActive(GameAction::kDPadUp) ||
                          IsActionActive(GameAction::kDPadDown) ||
                          IsActionActive(GameAction::kDPadLeft) ||
                          IsActionActive(GameAction::kDPadRight);

    if (dpad_key_held && (gsThumbRX != 0 || gsThumbRY != 0)) {
      int64_t last_motion_ms = g_last_real_mouse_motion_ms.load(std::memory_order_relaxed);
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
      constexpr int64_t kRecentMouseMotionWindowMs = 100;
      bool recent_real_mouse_motion = (now_ms - last_motion_ms) < kRecentMouseMotionWindowMs;

      if (!recent_real_mouse_motion) {
        int16_t suppressed_rx = gsThumbRX;
        int16_t suppressed_ry = gsThumbRY;
        if (IsValidGuestWritePtr(base, state_guest_ptr + 12, 2)) {
          GuestStoreU16(base, state_guest_ptr + 12, 0);
        }
        if (IsValidGuestWritePtr(base, state_guest_ptr + 14, 2)) {
          GuestStoreU16(base, state_guest_ptr + 14, 0);
        }

        static auto s_last_suppress_log = std::chrono::steady_clock::time_point{};
        auto suppress_now = std::chrono::steady_clock::now();
        if (suppress_now - s_last_suppress_log >= std::chrono::milliseconds(500)) {
          s_last_suppress_log = suppress_now;
          REXLOG_INFO(
              "[InputBindings] [DIAG] [DpadStickSuppress] zeroed right-stick "
              "(was RX={} RY={}) -- D-pad key held, no real mouse motion in "
              "the last {}ms",
              suppressed_rx, suppressed_ry, kRecentMouseMotionWindowMs);
        }
      }
    }
  }

  // 2026-11-15 raw mouse look (raw_mouse.h): runs after the SDK, the button
  // override and the D-pad/right-stick suppression above, and (while active)
  // fully owns sThumbRX/sThumbRY -- linear, framerate-independent mouse look
  // instead of the SDK's mouse->stick emulation.
  // Otherwise (controller, raw mouse off/inactive) the right stick is divided
  // by fable2_cam_rate_scale to undo the patched camera Lua's K x max rates.
  if (xam_status == 0 && state_guest_ptr != 0 &&
      IsValidGuestWritePtr(base, state_guest_ptr + 12, 4)) {
    if (!fable2::raw_mouse::ApplyToGuestState(user_index, state_guest_ptr, xam_status, base,
                                              IsKbmActive())) {
      fable2::raw_mouse::CompensateRightStick(state_guest_ptr, base);
    }
    gsThumbRX = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 12));
    gsThumbRY = static_cast<int16_t>(GuestLoadU16(base, state_guest_ptr + 14));
  }

  // Track connection state transitions for diagnostic logging
  static bool s_last_host_connected[4] = {false, false, false, false};
  if (user_index < 4 && host_connected != s_last_host_connected[user_index]) {
    s_last_host_connected[user_index] = host_connected;
    REXLOG_INFO("[GlyphSystem] [DIAG] Controller slot {} connection status changed: connected={}, host_ret=0x{:X}",
                user_index, host_connected, host_ret);
  }

  // Periodic diagnostic heartbeat log (once every 1 second)
  static auto s_last_diag_time = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (user_index == 0 && (now - s_last_diag_time) >= std::chrono::seconds(1)) {
    s_last_diag_time = now;
#if defined(_WIN32)
    REXLOG_INFO("[GlyphSystem] [DIAG] Poll slot 0: Host(connected={}, ret=0x{:X}, btn=0x{:04X}, LT={}, RT={}, LX={}, LY={}, RX={}, RY={}) | Guest(ret=0x{:X}, ptr=0x{:08X}, btn=0x{:04X}, LT={}, RT={}, LX={}, LY={}, RX={}, RY={}) | Active={}",
                host_connected, host_ret,
                host_state.Gamepad.wButtons, host_state.Gamepad.bLeftTrigger, host_state.Gamepad.bRightTrigger,
                host_state.Gamepad.sThumbLX, host_state.Gamepad.sThumbLY, host_state.Gamepad.sThumbRX, host_state.Gamepad.sThumbRY,
                xam_status, state_guest_ptr, gwButtons, gbLeftTrigger, gbRightTrigger, gsThumbLX, gsThumbLY, gsThumbRX, gsThumbRY,
                IsKbmActive() ? "KBM" : "Controller");
#endif
  }

  // REQUIREMENT 1: A disconnected controller can NEVER switch the active device to Controller.
  if (!host_connected) {
    // If guest state has non-zero inputs while host controller is disconnected, log that it was ignored
    static auto s_last_ignore_log = std::chrono::steady_clock::now();
    bool guest_has_input = (gwButtons != 0 || gbLeftTrigger > 30 || gbRightTrigger > 30 ||
                            std::abs(gsThumbLX) > 7849 || std::abs(gsThumbLY) > 7849 ||
                            std::abs(gsThumbRX) > 8689 || std::abs(gsThumbRY) > 8689);
    if (guest_has_input && (now - s_last_ignore_log) >= std::chrono::milliseconds(500)) {
      s_last_ignore_log = now;
      REXLOG_INFO("[GlyphSystem] [DIAG] Ignored guest gamepad input (synthetic KBM or uninitialized): guest_btn=0x{:04X}, LT={}, RT={}, LX={}, LY={}, RX={}, RY={}",
                  gwButtons, gbLeftTrigger, gbRightTrigger, gsThumbLX, gsThumbLY, gsThumbRX, gsThumbRY);
    }
    return;
  }

  // REQUIREMENT 2 & 3:
  // Polling alone does NOT count as activity.
  // ONLY real button/stick/trigger input from a successfully connected controller can switch the active device.
#if defined(_WIN32)
  const auto& pad = host_state.Gamepad;
  // 2026-11-15 live glyph switching, hysteresis: count only controller
  // ACTIVITY, edge-triggered per slot -- a newly pressed button, a trigger
  // crossing 30, or a stick beyond 8000 that just left the deadzone or is
  // still moving (>1500 since the last poll). A stick resting/drifting past
  // the deadzone, or a button merely held, can't keep stealing the device
  // back from a mouse/keyboard the player is actively using.
  constexpr int kStickActivity = 8000;
  constexpr int kStickMoveDelta = 1500;
  constexpr int kTriggerActivity = 30;
  struct PadEdgeState {
    uint16_t buttons = 0;
    bool lt = false, rt = false;
    int16_t sx[4] = {0, 0, 0, 0};
  };
  static PadEdgeState s_pad_edge[4];
  PadEdgeState& edge = s_pad_edge[user_index < 4 ? user_index : 0];
  const bool lt_now = pad.bLeftTrigger > kTriggerActivity;
  const bool rt_now = pad.bRightTrigger > kTriggerActivity;
  bool real_button = (pad.wButtons & ~edge.buttons) != 0;
  bool real_trigger = (lt_now && !edge.lt) || (rt_now && !edge.rt);
  bool real_stick = false;
  const int16_t sticks[4] = {pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY};
  for (int i = 0; i < 4; ++i) {
    const int now_v = sticks[i];
    const int prev_v = edge.sx[i];
    if (std::abs(now_v) > kStickActivity &&
        (std::abs(prev_v) <= kStickActivity || std::abs(now_v - prev_v) > kStickMoveDelta)) {
      real_stick = true;
    }
    edge.sx[i] = sticks[i];
  }
  edge.buttons = pad.wButtons;
  edge.lt = lt_now;
  edge.rt = rt_now;

  if (real_button || real_trigger || real_stick) {
    // [PERF-1006b]: this ran (and logged) on every poll while a stick was held,
    // on the game's own input thread. Log only the KBM -> controller switch.
    if (IsKbmActive()) {
      REXLOG_INFO("[GlyphSystem] [DIAG] REAL controller input detected on slot {}: btn=0x{:04X}, LT={}, RT={}, LX={}, LY={}, RX={}, RY={}",
                  user_index, pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger, pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY);
    }
    SetActiveInputDevice(InputDevice::kController);
  }
#endif
}

bool IsValidGuestReadPtr(const uint8_t* base, uint32_t guest_addr, size_t size) {
  if (base == nullptr || guest_addr < 0x10000) return false;
#if defined(_WIN32)
  const void* host_ptr = base + guest_addr;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(host_ptr, &mbi, sizeof(mbi)) != 0 &&
      mbi.State == MEM_COMMIT &&
      (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
      !(mbi.Protect & PAGE_GUARD)) {
    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (reinterpret_cast<uintptr_t>(host_ptr) + size <= region_end) return true;
  }
  if (guest_addr >= 0xA0000000 && guest_addr < 0xC0000000) {
    uint32_t phys_off = guest_addr - 0xA0000000;
    const void* phys_ptr = base + 0x100000000ull + phys_off;
    if (VirtualQuery(phys_ptr, &mbi, sizeof(mbi)) != 0 &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
        !(mbi.Protect & PAGE_GUARD)) {
      uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      return (reinterpret_cast<uintptr_t>(phys_ptr) + size <= region_end);
    }
  }
  return false;
#else
  return true;
#endif
}

bool IsValidGuestWritePtr(const uint8_t* base, uint32_t guest_addr, size_t size) {
  if (base == nullptr || guest_addr < 0x10000) return false;
#if defined(_WIN32)
  const void* host_ptr = base + guest_addr;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(host_ptr, &mbi, sizeof(mbi)) != 0 &&
      mbi.State == MEM_COMMIT &&
      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
      !(mbi.Protect & PAGE_GUARD)) {
    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (reinterpret_cast<uintptr_t>(host_ptr) + size <= region_end) return true;
  }
  if (guest_addr >= 0xA0000000 && guest_addr < 0xC0000000) {
    uint32_t phys_off = guest_addr - 0xA0000000;
    const void* phys_ptr = base + 0x100000000ull + phys_off;
    if (VirtualQuery(phys_ptr, &mbi, sizeof(mbi)) != 0 &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
        !(mbi.Protect & PAGE_GUARD)) {
      uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      return (reinterpret_cast<uintptr_t>(phys_ptr) + size <= region_end);
    }
  }
  return false;
#else
  return true;
#endif
}

struct ButtonBnkDef {
  ButtonType type;
  uint32_t bnk_offset;
  uint32_t width;
  uint32_t height;
  uint32_t bpp;
  bool tiled;
  const char* format_str;
};

static const ButtonBnkDef kButtonBnkDefs[] = {
  { ButtonType::kA,        0x013878, 64, 64, 4, true, "RGBA8 (fmt=0xa2)" },
  { ButtonType::kB,        0x017d38, 64, 64, 4, true, "RGBA8 (fmt=0xa2)" },
  { ButtonType::kX,        0x006720, 64, 64, 4, true, "RGBA8 (fmt=0xa2)" },
  { ButtonType::kY,        0x001e98, 64, 64, 4, true, "RGBA8 (fmt=0xa2)" },
  { ButtonType::kRB,       0x00de10, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kLT,       0x00da48, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kDPad,     0x0082a0, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kStart,    0x006fb8, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kBack,     0x008508, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kLB,       0x0119e0, 32, 32, 1, true, "A8 (fmt=0xb0)" },
  { ButtonType::kRT,       0x004938, 32, 32, 1, true, "A8 (fmt=0xb0)" },
};

// DIAGNOSTIC helper (2026-09-13, fable2_048.log round): decode 3 already-
// big-endian dwords as a candidate xe_gpu_texture_fetch_t and check it
// against EVERY known button's own dimensions/tiling (not just one button
// at a time, as the original per-header scan in OnResourceResolved does) --
// used by AnalyzeBnkMatchContext() below when following candidate pointers
// found near a located name-hash table entry, so a hit against ANY of the
// 11 known buttons is recognized regardless of which button's hash led us
// there.
bool MatchesAnyKnownFetchConstant(uint32_t d0, uint32_t d1, uint32_t d2,
                                   uint32_t* out_base_addr, const ButtonBnkDef** out_def) {
  uint32_t base_addr = d1 & 0xFFFFF000;
  if (base_addr < 0x10000) return false;
  bool tiled = (d0 >> 31) & 1;
  uint32_t w_minus1 = d2 & 0x1FFF;
  uint32_t h_minus1 = (d2 >> 13) & 0x1FFF;
  for (const auto& d : kButtonBnkDefs) {
    if (w_minus1 + 1 == d.width && h_minus1 + 1 == d.height && tiled == d.tiled) {
      if (out_base_addr) *out_base_addr = base_addr;
      if (out_def) *out_def = &d;
      return true;
    }
  }
  return false;
}

// Unique 24-byte signature of the A-button 84-byte header in gui_texture_headers.bnk (offset 0x1B878 + 12):
// words 3..8: fmt=0xa2, w=64, h=64, mips=2, 1, sz=84
static const uint8_t kAButtonHeaderSigBE[24] = {
  0x00, 0x00, 0x00, 0xa2, 0x00, 0x00, 0x00, 0x40,
  0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x02,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x54
};
static const uint8_t kAButtonHeaderSigLE[24] = {
  0xa2, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
  0x40, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00
};

static uint32_t g_bnk_guest_base = 0;

static const uint8_t* GetHostReadPtr(const uint8_t* base, uint32_t guest_addr) {
  if (!base || guest_addr < 0x10000) return nullptr;
  if (guest_addr >= 0xA0000000 && guest_addr < 0xC0000000) {
    return base + 0x100000000ull + (guest_addr - 0xA0000000);
  }
  if (guest_addr >= 0xC0000000 && guest_addr < 0xE0000000) {
    return base + 0x100000000ull + (guest_addr - 0xC0000000);
  }
  return base + guest_addr;
}

static void ReadGuestPixels(const uint8_t* base, uint32_t guest_addr, uint8_t* dst, size_t size) {
  if (!base || guest_addr < 0x10000 || !dst || size == 0) return;
  const uint8_t* src = GetHostReadPtr(base, guest_addr);
  if (src) {
    std::memcpy(dst, src, size);
  } else {
    std::memcpy(dst, base + guest_addr, size);
  }
}

static void WriteGuestPixels(uint8_t* base, uint32_t guest_addr, const uint8_t* data, size_t size) {
  if (!base || guest_addr < 0x10000 || !data || size == 0) return;
  std::memcpy(base + guest_addr, data, size);
  if (guest_addr >= 0xA0000000 && guest_addr < 0xC0000000) {
    uint32_t phys_off = guest_addr - 0xA0000000;
    std::memcpy(base + 0x100000000ull + phys_off, data, size);
    std::memcpy(base + 0xC0000000 + phys_off, data, size);
  } else if (guest_addr >= 0xC0000000 && guest_addr < 0xE0000000) {
    uint32_t phys_off = guest_addr - 0xC0000000;
    std::memcpy(base + 0x100000000ull + phys_off, data, size);
    std::memcpy(base + 0xA0000000 + phys_off, data, size);
  }
}

uint32_t LocateBnkBaseInMemory(const uint8_t* base) {
  if (g_bnk_guest_base != 0) {
    uint32_t a_addr = g_bnk_guest_base + 0x013878;
    if (IsValidGuestReadPtr(base, a_addr + 12, 24)) {
      const uint8_t* ptr = GetHostReadPtr(base, a_addr + 12);
      if (ptr && (std::memcmp(ptr, kAButtonHeaderSigBE, 24) == 0 ||
                  std::memcmp(ptr, kAButtonHeaderSigLE, 24) == 0)) {
        return g_bnk_guest_base;
      }
    }
    return g_bnk_guest_base;
  }

#if defined(_WIN32)
  static bool s_logged_scan = false;
  if (!s_logged_scan) {
    s_logged_scan = true;
    REXLOG_INFO("[GlyphSystem] [DIAG] Locating gui_texture_headers.bnk in guest/physical RAM (base={:p})...", fmt::ptr(base));
  }

  // 1. Search Physical Memory raw heap (mapping_base_ + 0x100000000ull, 512MB)
  const uint8_t* phys_base = base + 0x100000000ull;
  MEMORY_BASIC_INFORMATION phys_mbi{};
  if (VirtualQuery(phys_base, &phys_mbi, sizeof(phys_mbi)) != 0 &&
      phys_mbi.State == MEM_COMMIT &&
      (phys_mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {

    size_t phys_len = std::min<size_t>(phys_mbi.RegionSize, 0x20000000ull);
    for (size_t i = 0; i + 24 <= phys_len; i += 4) {
      uint32_t val = *reinterpret_cast<const uint32_t*>(phys_base + i);

      // Check face button header signatures (fmt=0xa2, w=64, h=64, mips=2, 1, sz=84)
      // When memory has BE bytes (00 00 00 a2): on x86, val is 0xa2000000
      // When memory has LE bytes (a2 00 00 00): on x86, val is 0x000000a2
      bool match_be = (val == 0xa2000000 && std::memcmp(phys_base + i, kAButtonHeaderSigBE, 24) == 0);
      bool match_le = (val == 0x000000a2 && std::memcmp(phys_base + i, kAButtonHeaderSigLE, 24) == 0);

      if (match_be || match_le) {
        const uint8_t* sig = match_be ? kAButtonHeaderSigBE : kAButtonHeaderSigLE;
        uint32_t sig_phys = static_cast<uint32_t>(i);
        // Face button offsets within data section: Y (0x1E98), X (0x6720), A (0x13878), B (0x17D38)
        static const uint32_t kFaceBtnOffsets[] = { 0x001E98, 0x006720, 0x013878, 0x017D38 };
        for (uint32_t cand_btn_off : kFaceBtnOffsets) {
          if (sig_phys >= cand_btn_off + 12) {
            uint32_t cand_data_base = sig_phys - 12 - cand_btn_off;
            if (cand_data_base + 0x013878 + 36 <= phys_len &&
                cand_data_base + 0x001E98 + 36 <= phys_len) {
              if (std::memcmp(phys_base + cand_data_base + 0x013878 + 12, sig, 24) == 0 &&
                  std::memcmp(phys_base + cand_data_base + 0x001E98 + 12, sig, 24) == 0) {
                g_bnk_guest_base = 0xA0000000 + cand_data_base;
                REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in physical RAM via multi-button cross-check ({}): phys_base=0x{:08X}, bnk_guest_base=0x{:08X}",
                            match_be ? "BE" : "LE", cand_data_base, g_bnk_guest_base);
                return g_bnk_guest_base;
              }
            }
          }
        }
      }

      // Check gui_texture_headers.bnk file header (50 blocks = 0x32): w0=0x8000, w1=3, w2=0x32
      // In BE memory (00 00 80 00  00 00 00 03  00 00 00 32):
      // On x86 (little endian host), word 0 is 0x00800000, word 1 is 0x03000000, word 2 is 0x32000000.
      if (val == 0x00800000 && *reinterpret_cast<const uint32_t*>(phys_base + i + 4) == 0x03000000 &&
                               *reinterpret_cast<const uint32_t*>(phys_base + i + 8) == 0x32000000) {
        uint32_t phys_off = static_cast<uint32_t>(i);
        g_bnk_guest_base = 0xA0000000 + phys_off + 0x8000;
        REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in physical RAM via file header BE: phys_off=0x{:08X}, bnk_guest_base=0x{:08X}",
                    phys_off, g_bnk_guest_base);
        return g_bnk_guest_base;
      } else if (val == 0x00008000 && *reinterpret_cast<const uint32_t*>(phys_base + i + 4) == 0x00000003 &&
                                      *reinterpret_cast<const uint32_t*>(phys_base + i + 8) == 0x00000032) {
        uint32_t phys_off = static_cast<uint32_t>(i);
        g_bnk_guest_base = 0xA0000000 + phys_off + 0x8000;
        REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in physical RAM via file header LE: phys_off=0x{:08X}, bnk_guest_base=0x{:08X}",
                    phys_off, g_bnk_guest_base);
        return g_bnk_guest_base;
      }
    }
  }

  // 2. Search Guest Virtual Heaps
  const std::pair<uintptr_t, uintptr_t> v_ranges[] = {
    { 0x40000000, 0x7F000000 },
    { 0x80000000, 0xA0000000 },
    { 0x00010000, 0x40000000 },
    { 0xA0000000, 0xC0000000 },
    { 0xC0000000, 0xE0000000 },
    { 0xE0000000, 0xFFFFFFFF },
  };

  for (const auto& [range_start, range_end] : v_ranges) {
    uintptr_t cur_host = reinterpret_cast<uintptr_t>(base + range_start);
    uintptr_t end_host = reinterpret_cast<uintptr_t>(base + range_end);

    while (cur_host < end_host) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<const void*>(cur_host), &mbi, sizeof(mbi)) == 0) break;

      uintptr_t region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      uintptr_t region_end = region_base + mbi.RegionSize;
      uintptr_t scan_start = std::max(cur_host, region_base);
      uintptr_t scan_end = std::min(end_host, region_end);

      if (mbi.State == MEM_COMMIT &&
          (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
          !(mbi.Protect & PAGE_GUARD)) {

        if (scan_end >= scan_start + 24) {
          const uint8_t* start_ptr = reinterpret_cast<const uint8_t*>(scan_start);
          size_t scan_len = scan_end - scan_start;

          for (size_t i = 0; i + 24 <= scan_len; i += 4) {
            uint32_t val = *reinterpret_cast<const uint32_t*>(start_ptr + i);

            bool match_be = (val == 0xa2000000 && std::memcmp(start_ptr + i, kAButtonHeaderSigBE, 24) == 0);
            bool match_le = (val == 0x000000a2 && std::memcmp(start_ptr + i, kAButtonHeaderSigLE, 24) == 0);

            if (match_be || match_le) {
              const uint8_t* sig = match_be ? kAButtonHeaderSigBE : kAButtonHeaderSigLE;
              uintptr_t sig_host = scan_start + i;
              uint32_t sig_guest = static_cast<uint32_t>(sig_host - reinterpret_cast<uintptr_t>(base));

              static const uint32_t kFaceBtnOffsets[] = { 0x001E98, 0x006720, 0x013878, 0x017D38 };
              for (uint32_t cand_btn_off : kFaceBtnOffsets) {
                if (sig_guest >= cand_btn_off + 12) {
                  uint32_t cand_data_base = sig_guest - 12 - cand_btn_off;
                  uint32_t check_a = cand_data_base + 0x013878 + 12;
                  uint32_t check_y = cand_data_base + 0x001E98 + 12;
                  if (IsValidGuestReadPtr(base, check_a, 24) && IsValidGuestReadPtr(base, check_y, 24)) {
                    const uint8_t* ptr_a = GetHostReadPtr(base, check_a);
                    const uint8_t* ptr_y = GetHostReadPtr(base, check_y);
                    if (ptr_a && ptr_y &&
                        std::memcmp(ptr_a, sig, 24) == 0 &&
                        std::memcmp(ptr_y, sig, 24) == 0) {
                      g_bnk_guest_base = cand_data_base;
                      REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in virtual RAM via multi-button cross-check ({}): guest_base=0x{:08X}",
                                  match_be ? "BE" : "LE", g_bnk_guest_base);
                      return g_bnk_guest_base;
                    }
                  }
                }
              }
            }

            if (val == 0x00800000 && *reinterpret_cast<const uint32_t*>(start_ptr + i + 4) == 0x03000000 &&
                                     *reinterpret_cast<const uint32_t*>(start_ptr + i + 8) == 0x32000000) {
              uintptr_t found_host = scan_start + i;
              uint32_t found_guest = static_cast<uint32_t>(found_host - reinterpret_cast<uintptr_t>(base));
              g_bnk_guest_base = found_guest + 0x8000;
              REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in virtual RAM via file header BE: guest_addr=0x{:08X}, bnk_guest_base=0x{:08X}",
                          found_guest, g_bnk_guest_base);
              return g_bnk_guest_base;
            } else if (val == 0x00008000 && *reinterpret_cast<const uint32_t*>(start_ptr + i + 4) == 0x00000003 &&
                                            *reinterpret_cast<const uint32_t*>(start_ptr + i + 8) == 0x00000032) {
              uintptr_t found_host = scan_start + i;
              uint32_t found_guest = static_cast<uint32_t>(found_host - reinterpret_cast<uintptr_t>(base));
              g_bnk_guest_base = found_guest + 0x8000;
              REXLOG_INFO("[GlyphSystem] [DIAG] Located gui_texture_headers.bnk in virtual RAM via file header LE: guest_addr=0x{:08X}, bnk_guest_base=0x{:08X}",
                          found_guest, g_bnk_guest_base);
              return g_bnk_guest_base;
            }
          }
        }
      }
      cur_host = region_end;
    }
  }
#endif

  return 0;
}

// DIAGNOSTIC (2026-09-12, fable2_047.log round): now that resource_guest_ptr
// is known to be GameNameHash32(full_resource_path) rather than a pointer
// (see GameNameHash32 above), the working theory for where the REAL
// GPU-facing texture lives is that gui_texture_headers.bnk (already
// locatable via LocateBnkBaseInMemory above) contains a hash-indexed lookup
// table mapping this same hash to an offset/address/descriptor -- the
// per-button `bnk_offset` constants in kButtonBnkDefs[] were originally
// reverse-engineered by hand from a raw byte scan in an earlier round, so it
// is plausible they are actually the RESULT of exactly this kind of hash
// lookup rather than fixed, independent compile-time constants. If the raw
// hash value (in either byte order) appears anywhere in the bank's memory,
// that is the critical clue toward the real hash -> texture-descriptor
// table this investigation is now looking for.
//
// This function scans a bounded window of guest memory starting at the
// located bank base for the literal 32-bit hash value, in both possible
// byte orders -- the bank's guest memory is big-endian Xbox 360 data, but
// `hash_value` here is computed in native/host byte order by
// GameNameHash32(), so both the value itself and its byte-swapped form are
// checked against each raw (untouched, non-byteswapped) 4-byte-aligned
// guest word. The ~1MB window comfortably covers gui_texture_headers.bnk's
// on-disk size (135,460 bytes, confirmed via device_list_dir this round)
// with generous headroom for any in-RAM expansion/padding, and the scan
// stops early the moment it walks off the end of valid/committed guest
// memory (IsValidGuestReadPtr), which in practice will almost always be
// well before the full window is consumed. Strictly read-only: never
// writes. Gated to run at most once per distinct hash value ever seen
// across the process lifetime (a small cached set), so a texture resolved
// repeatedly (icon_button_a.tex resolves once per menu open, per the
// existing occurrence counters elsewhere in this file) does not re-scan
// megabytes of memory on every single call.
constexpr size_t kBnkScanWindowBytes = 1 * 1024 * 1024; // ~1MB; see comment above.

// DIAGNOSTIC (2026-09-13, fable2_048.log round): `fable2_048.log` proved
// this theory correct -- `GameNameHash32("art\\gui\\controller\\
// icon_button_a.tex") == 0x448CC960` occurs (byte-swapped) exactly once in
// the located `gui_texture_headers.bnk` memory, at bank offset 0x1A8EC.
// This function replaces the original round-4 narrow +/-3-word context peek
// with a much wider (kSideWords each side), fully-decoded window dump PLUS
// active pointer-following: every word in that window that looks like a
// plausible guest pointer is dereferenced and checked against (a) the same
// 24-byte per-button header signature LocateBnkBaseInMemory() uses (does
// this word point AT a header record directly?), and (b) a valid
// xe_gpu_texture_fetch_t for ANY known button's dimensions (does this word
// point at a real GPU texture-fetch constant?) -- see
// MatchesAnyKnownFetchConstant() above. This is the systematic
// "follow the returned object forward toward the actual texture payload"
// step the investigation needs, rather than another isolated print.
// Strictly read-only; bounded by construction (a fixed-size window, so a
// fixed, small number of candidate dereferences per match).
void AnalyzeBnkMatchContext(const uint8_t* base, uint32_t match_guest_addr) {
  constexpr int kSideWords = 32; // 128 bytes each side of the match word
  constexpr int kTotalWords = kSideWords * 2; // excludes the match word itself (already logged by the caller)
  uint32_t words[kTotalWords] = {0};
  bool valid[kTotalWords] = {false};
  int32_t rel[kTotalWords] = {0};

  for (int i = 0; i < kTotalWords; ++i) {
    int32_t off_words = (i < kSideWords) ? (i - kSideWords) : (i - kSideWords + 1); // -32..-1, +1..+32
    rel[i] = off_words;
    uint32_t addr = static_cast<uint32_t>(match_guest_addr + off_words * 4);
    if (IsValidGuestReadPtr(base, addr, 4)) {
      const uint32_t* p = reinterpret_cast<const uint32_t*>(GetHostReadPtr(base, addr));
      if (!p) p = reinterpret_cast<const uint32_t*>(base + addr);
      words[i] = __builtin_bswap32(*p);
      valid[i] = true;
    }
  }

  REXLOG_INFO("[GlyphSystem] [DIAG3]   wide context dump ({} words each side of guest_addr=0x{:08X}, decoded big-endian, '?\?' = unmapped word):",
              kSideWords, match_guest_addr);
  for (int line = 0; line < kTotalWords; line += 8) {
    REXLOG_INFO("[GlyphSystem] [DIAG3]     rel[{:+d}..{:+d}]: {}{:08X} {}{:08X} {}{:08X} {}{:08X} {}{:08X} {}{:08X} {}{:08X} {}{:08X}",
                rel[line], rel[line + 7],
                valid[line + 0] ? "0x" : "??", words[line + 0],
                valid[line + 1] ? "0x" : "??", words[line + 1],
                valid[line + 2] ? "0x" : "??", words[line + 2],
                valid[line + 3] ? "0x" : "??", words[line + 3],
                valid[line + 4] ? "0x" : "??", words[line + 4],
                valid[line + 5] ? "0x" : "??", words[line + 5],
                valid[line + 6] ? "0x" : "??", words[line + 6],
                valid[line + 7] ? "0x" : "??", words[line + 7]);
  }

  // Follow every window word that looks like a plausible guest pointer.
  int follow_hits = 0;
  constexpr int kMaxFollowHitsLogged = 4;
  for (int i = 0; i < kTotalWords; ++i) {
    if (!valid[i]) continue;
    uint32_t cand = words[i];
    if (cand < 0x10000) continue; // never a plausible pointer

    // (a) does `cand` itself host a per-button header signature at +12
    // (the same layout LocateBnkBaseInMemory's own signature scan uses)?
    if (IsValidGuestReadPtr(base, cand + 12, 24)) {
      const uint8_t* sig_ptr = GetHostReadPtr(base, cand + 12);
      if (!sig_ptr) sig_ptr = base + cand + 12;
      if (std::memcmp(sig_ptr, kAButtonHeaderSigBE, 24) == 0 ||
          std::memcmp(sig_ptr, kAButtonHeaderSigLE, 24) == 0) {
        ++follow_hits;
        if (follow_hits <= kMaxFollowHitsLogged) {
          REXLOG_INFO("[GlyphSystem] [DIAG3]   ** POINTER FOLLOW HIT ** rel[{:+d}]=0x{:08X} directly hosts a per-button header signature at +12 (looks like a header-record pointer)",
                      rel[i], cand);
        }
      }
    }

    // (b) does a small window at `cand` decode as a valid
    // xe_gpu_texture_fetch_t for ANY known button's dimensions?
    if (IsValidGuestReadPtr(base, cand, 96)) {
      const uint32_t* wp = reinterpret_cast<const uint32_t*>(GetHostReadPtr(base, cand));
      if (!wp) wp = reinterpret_cast<const uint32_t*>(base + cand);
      uint32_t ww[24];
      for (int k = 0; k < 24; ++k) ww[k] = __builtin_bswap32(wp[k]);
      for (int s = 0; s + 3 <= 24; ++s) {
        uint32_t out_base_addr = 0;
        const ButtonBnkDef* out_def = nullptr;
        if (MatchesAnyKnownFetchConstant(ww[s], ww[s + 1], ww[s + 2], &out_base_addr, &out_def)) {
          ++follow_hits;
          if (follow_hits <= kMaxFollowHitsLogged) {
            REXLOG_INFO("[GlyphSystem] [DIAG3]   ** POINTER FOLLOW HIT ** rel[{:+d}]=0x{:08X} -> word[{}] decodes as xe_gpu_texture_fetch_t: base_address=0x{:08X} {}x{} tiled={} (matches known button '{}')",
                        rel[i], cand, s, out_base_addr, out_def->width, out_def->height, out_def->tiled,
                        ButtonTypeName(out_def->type));
          }
        }
      }
    }
  }
  if (follow_hits > 0) {
    REXLOG_INFO("[GlyphSystem] [DIAG3]   pointer-follow summary: {} total hit(s) across {} candidate window word(s) ({} logged in detail above).",
                follow_hits, kTotalWords, std::min(follow_hits, kMaxFollowHitsLogged));
  }
}

// Known / candidate resource-name hashes to proactively search for inside
// gui_texture_headers.bnk once it's located. CONFIRMED entries were directly
// observed as a real resource_guest_ptr/res_id value in an actual capture
// (and, for DPad, independently re-derived offline in the fable2_048.log
// round); PREDICTED-then-CONFIRMED: B was only a guessed, never-observed
// name going into the fable2_050.log round, but that round's scan found a
// literal, exact match for it inside the bank too -- see the
// fable2_050.log-round section of glyph-vfs-redirect-status.md. X/Y remain
// unverified guesses (by the same A/B naming analogy) pending a scan result.
// See that doc for exactly how each value below was derived.
struct KnownHashProbe { uint32_t hash; const char* label; };
constexpr KnownHashProbe kKnownResourceHashProbes[] = {
  { 0x448CC960u, "art\\gui\\controller\\icon_button_a.tex (A) [CONFIRMED: live resolve + bank scan, fable2_048/050.log]" },
  { 0xD7485757u, "art\\gui\\controller\\24x24\\dpad.tex (DPad) [CONFIRMED: fable2_045.log observed hash + fable2_048.log offline string match + fable2_050.log bank scan]" },
  { 0xB5E328DFu, "art\\gui\\controller\\icon_button_b.tex (B) [CONFIRMED via fable2_050.log bank scan -- predicted-by-analogy name now verified]" },
  { 0xD50C2A71u, "art\\gui\\controller\\icon_button_x.tex (X) [PREDICTED, unverified -- never observed live or in a bank scan yet]" },
  { 0xD712D1A8u, "art\\gui\\controller\\icon_button_y.tex (Y) [PREDICTED, unverified -- never observed live or in a bank scan yet]" },
};

// One-shot launch guard for RunBankHashScanBackground() below -- see its own
// doc comment for the full fable2_050.log-round performance-fix rationale.
std::atomic<bool> g_bank_scan_launched{false};

// PERFORMANCE FIX (2026-09-13, fable2_050.log round): this REPLACES round
// 5's ScanBnkForNameHash(), which scanned the full ~1MB window SEPARATELY
// for EACH of the (up to) 5 known hashes, SYNCHRONOUSLY on the game's own
// resolve-call thread (OnResourceResolved runs as part of the recompiled
// PPC code's own call chain -- there is no separate "diagnostics thread" in
// this codebase; whatever runs inline here blocks the game itself).
// fable2_050.log proved this cost ~11-16 seconds PER hash (confirmed via
// log timestamps: the A scan took 11.06s, the DPad scan took 15.85s, and
// the B scan was still running when the user gave up and closed the game --
// the title screen never progressed past the splash). That is a
// performance regression this investigation introduced, not a game bug.
// Two changes fix it:
//   1. ONE single linear pass over the window checks EVERY known hash at
//      once via an O(1) hash-set lookup per word, instead of N separate
//      full passes -- this alone cuts the total scan cost by roughly Nx.
//   2. That single pass now runs on a detached background thread, launched
//      at most once ever (guarded by g_bank_scan_launched), so the game's
//      own simulation/resolve thread never blocks on it at all. The
//      previous version's fundamental mistake was doing 100% of this work
//      inline on the one thread the game itself needs to keep running.
// The bounded pointer-follow/context analysis (AnalyzeBnkMatchContext) still
// runs, but only once per actual match found (5 or fewer expected per
// process), which is cheap regardless of which thread runs it -- the ~1MB
// linear memory scan was always the expensive part, not the analysis.
// REXLOG_INFO is assumed thread-safe for concurrent logging from multiple
// threads (standard for this class of logging library); this function
// performs no writes to guest memory and touches no shared state besides
// its own local set and the one-shot launch flag, so no additional locking
// is needed beyond that.
// PERFORMANCE FIX (2026-09-12, fable2_051.log round): the loop below used to
// call IsValidGuestReadPtr() once per 4-byte word -- 262144 calls for the
// full 1MB window -- and every call costs a Win32 VirtualQuery() syscall.
// fable2_051.log proved the round-6 background-thread fix successfully
// stopped the game from stalling (frame-paced [GlyphSystem] activity
// continues normally on the main thread the whole time), but the scan
// ITSELF still never finished producing a single [DIAG3] MATCH/complete
// line in the ~7 seconds the user waited before closing the game -- 262144
// VirtualQuery calls is still tens of thousands of syscalls, easily
// multiple seconds of pure syscall overhead even on a background thread.
// Fixed by querying the valid/committed range in region-sized strides
// (VirtualQuery reports the whole containing region's size, so one call
// covers potentially the entire window) instead of one call per word --
// see ComputeSafeReadableWindowBytes() below.
size_t ComputeSafeReadableWindowBytes(const uint8_t* base, uint32_t start_guest_addr, size_t max_bytes) {
#if defined(_WIN32)
  size_t safe_bytes = 0;
  uint32_t addr = start_guest_addr;
  while (safe_bytes < max_bytes) {
    const uint8_t* host_ptr = GetHostReadPtr(base, addr);
    if (!host_ptr) host_ptr = base + addr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(host_ptr, &mbi, sizeof(mbi)) == 0) break;
    if (mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ||
        (mbi.Protect & PAGE_GUARD)) {
      break;
    }
    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t host_addr = reinterpret_cast<uintptr_t>(host_ptr);
    if (region_end <= host_addr) break; // shouldn't happen, but never spin on it
    size_t region_remaining = static_cast<size_t>(region_end - host_addr);
    size_t advance = std::min(region_remaining, max_bytes - safe_bytes);
    safe_bytes += advance;
    addr += static_cast<uint32_t>(advance);
  }
  return safe_bytes;
#else
  return max_bytes;
#endif
}

void RunBankHashScanBackground(const uint8_t* base, uint32_t bnk_base_probe) {
  if (bnk_base_probe == 0 || base == nullptr) return;
  if (g_bank_scan_launched.exchange(true, std::memory_order_acq_rel)) return; // already launched once

  std::thread([base, bnk_base_probe]() {
#if defined(_WIN32)
    // Best-effort: keep this background scan from competing with the game's
    // own threads for CPU time. Not load-bearing (the real fix is running
    // off the game thread at all, not thread priority) -- failing to set
    // this is harmless.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    REXLOG_INFO("[GlyphSystem] [DIAG3] Background bank hash scan starting on a worker thread (bnk_base=0x{:08X}, {} known hash(es), single {}-byte pass)...",
                bnk_base_probe, std::size(kKnownResourceHashProbes), kBnkScanWindowBytes);

    // PERFORMANCE FIX (fable2_051.log round): compute the safely-readable
    // window ONCE, in a handful of region-sized VirtualQuery calls, instead
    // of once per word -- see ComputeSafeReadableWindowBytes()'s doc comment
    // above. This is what actually made fable2_051.log's scan never finish:
    // 262144 per-word VirtualQuery syscalls, even off the game thread, is
    // still multiple seconds of pure kernel-transition overhead.
    size_t safe_bytes = ComputeSafeReadableWindowBytes(base, bnk_base_probe, kBnkScanWindowBytes);
    REXLOG_INFO("[GlyphSystem] [DIAG3] Safely-readable window: {} of {} requested bytes (bnk_base=0x{:08X}).",
                safe_bytes, kBnkScanWindowBytes, bnk_base_probe);

    // Build a lookup from raw 32-bit guest word (either byte order) -> the
    // probe it corresponds to, so the single pass below can check ALL known
    // hashes per word in O(1) instead of looping over the probe list.
    std::unordered_map<uint32_t, const KnownHashProbe*> target_lookup;
    for (const auto& probe : kKnownResourceHashProbes) {
      target_lookup[probe.hash] = &probe;
      target_lookup[__builtin_bswap32(probe.hash)] = &probe;
    }

    std::unordered_set<uint32_t> hashes_matched; // which probe hashes we've already logged a match for
    int total_matches = 0;
    constexpr int kMaxMatchesAnalyzedTotal = 24; // across ALL hashes combined this one pass

    // BOUNDED PROGRESS OUTPUT (2026-09-12, fable2_051.log round): log every
    // 1/8th of the window so a capture always shows the scan is alive and
    // making bounded, visible progress even if the process is closed before
    // it reaches the final "complete" line.
    constexpr size_t kProgressStrideBytes = 131072; // 128KB, 1/8 of the 1MB window
    size_t next_progress_at = kProgressStrideBytes;

    const uint8_t* host_base_ptr = GetHostReadPtr(base, bnk_base_probe);
    if (!host_base_ptr) host_base_ptr = base + bnk_base_probe;

    for (size_t off = 0; off + 4 <= safe_bytes; off += 4) {
      if (off >= next_progress_at) {
        REXLOG_INFO("[GlyphSystem] [DIAG3] ...scan progress: {}/{} bytes ({} match(es) so far).",
                    off, safe_bytes, total_matches);
        next_progress_at += kProgressStrideBytes;
      }

      uint32_t raw_word = *reinterpret_cast<const uint32_t*>(host_base_ptr + off);

      auto it = target_lookup.find(raw_word);
      if (it == target_lookup.end()) continue;

      uint32_t guest_addr = bnk_base_probe + static_cast<uint32_t>(off);
      const KnownHashProbe* probe = it->second;
      ++total_matches;
      hashes_matched.insert(probe->hash);
      bool native_order = (raw_word == probe->hash);
      if (total_matches <= kMaxMatchesAnalyzedTotal) {
        REXLOG_INFO("[GlyphSystem] [DIAG3] ** MATCH ** hash=0x{:08X} ('{}') guest_addr=0x{:08X} (bnk_off=0x{:08X}) ({})",
                    probe->hash, probe->label, guest_addr, off, native_order ? "native-order" : "byte-swapped");
        // Bounded, once per logged match (see AnalyzeBnkMatchContext's own
        // doc comment above): the wide-context dump plus pointer-follow is
        // the one part of this diagnostic that does meaningful extra work
        // per hit, so it stays capped by kMaxMatchesAnalyzedTotal regardless
        // of how many raw word matches a given hash produces.
        AnalyzeBnkMatchContext(base, guest_addr);
      }
    }

    for (const auto& probe : kKnownResourceHashProbes) {
      if (!hashes_matched.count(probe.hash)) {
        REXLOG_INFO("[GlyphSystem] [DIAG3] hash=0x{:08X} ('{}'): 0 match(es) found in this pass.", probe.hash, probe.label);
      }
    }
    REXLOG_INFO("[GlyphSystem] [DIAG3] Background bank hash scan complete: {} total match(es) across {} known hash(es), single pass of {} (of {} requested) bytes from bnk_base=0x{:08X}.",
                total_matches, std::size(kKnownResourceHashProbes), safe_bytes, kBnkScanWindowBytes, bnk_base_probe);
  }).detach();
}

// ---------------------------------------------------------------------------
// A-button REAL pixel-payload search + guarded write (2026-09-12,
// fable2_051.log round -- today's primary objective: get A's menu/card
// glyph to actually render as the PC "space" key instead of the Xbox green
// A, before session end).
//
// gui_texture_headers.bnk (everything above) is CONFIRMED, yet again this
// round, to be pure metadata: every header word past the 8-word signature
// is zero except one small constant field (=0xD for A) -- there is no GPU
// pointer, offset, or size field anywhere in it. So the actual pixel bytes
// must come from somewhere else. This section identifies exactly where,
// using a completely different, static/offline technique: a full binary
// diff of the two already-shipped, pre-repacked sibling banks
// (extracted/data/art/gui/gui_textures_kbm.bnk vs ..._x360.bnk -- the
// SAME banks the working _gui_kbm/_gui_x360 VFS directory redirect above
// already swaps between for title/HUD glyphs). Exactly 11 contiguous byte
// ranges differ between the two files, matching the "11 controller-glyph
// texture regions" the redirect's own doc comment describes, and sized
// exactly as expected: four 16383-byte clusters (64x64 RGBA8 face
// buttons) and seven 991-byte clusters (32x32 A8 icons). Which 64x64
// cluster is which button was identified by average RGB color in the
// UNTOUCHED x360 bytes -- swizzling/tiling only reorders WHERE pixels
// sit, never changes the SET of byte values written, so a simple mean
// over all 16384 bytes survives tiling intact. File offset 0x140D555
// (within gui_textures.bnk / _x360.bnk / _kbm.bnk -- all three share this
// layout except for the 11 swapped regions) averages to a dominant green
// -- the Xbox A-button's own color -- confirming it is A's tile (the
// other three 64x64 clusters average red/blue/yellow, matching B/X/Y
// respectively; all four/seven offsets are recorded in
// glyph-vfs-redirect-status.md for a future round covering the other
// buttons).
//
// IMPORTANT CAVEAT: this is a DIFFERENT file/coordinate-system from
// kButtonBnkDefs/bnk_base_probe above (those are gui_texture_headers.bnk;
// this is gui_textures.bnk, a separate, much larger bank that the file's
// own top comment says is loaded "on-demand" rather than resident as one
// block). Whether A's 16KB tile is ever memory-resident as a contiguous,
// findable copy AT ALL -- let alone near the header bank's own location --
// is NOT a prior fact, it is what this diagnostic exists to determine
// empirically. The search below is anchored at bnk_base_probe purely
// because it is the one already-located address this code has to start
// from; if gui_textures.bnk content lives in a completely separate
// memory arena, this search will legitimately find nothing, and that
// negative result is itself useful (rules out "simple co-located copy").
[[maybe_unused]] constexpr uint32_t kAButtonPixelFileOffset = 0x140D555;  // within gui_textures.bnk / _x360.bnk / _kbm.bnk -- documentary only, not used by the memory search below (which operates in a different, guest-memory coordinate space)
constexpr size_t kAButtonPixelPayloadSize = 16384;       // 64*64*4, RGBA8, tiled
constexpr size_t kAButtonPixelSigRelOffset = 12288;      // offset within the 16384-byte tile where the signature below was sampled

// 256-byte signature of A's ORIGINAL/X360 (stale, pre-redirect, Xbox-green)
// bytes at kAButtonPixelFileOffset + kAButtonPixelSigRelOffset, extracted
// directly from extracted/data/art/gui/gui_textures_x360.bnk (byte-
// identical to the untouched original gui_textures.bnk at this offset --
// verified offline). A match against THIS specific signature (not the KBM
// variant's own bytes) is exactly what the "menu caches the pre-redirect,
// still-Xbox-colored bytes and never re-fetches them" hypothesis predicts
// finding resident in guest memory while KBM mode is active.
static const uint8_t kAButtonX360PixelSig[256] = {
  0x65, 0x7C, 0x24, 0xFF, 0x65, 0x7C, 0x24, 0xFF, 0x62, 0x78, 0x22, 0xFF, 0x7F, 0x96, 0x38, 0xFF,
  0x90, 0xA7, 0x47, 0xFF, 0x91, 0xA9, 0x48, 0xFF, 0x95, 0xAD, 0x4C, 0xFF, 0xA4, 0xBC, 0x59, 0xFF,
  0xBE, 0xD6, 0x71, 0xFF, 0xCB, 0xE4, 0x7E, 0xFF, 0xCB, 0xE4, 0x7E, 0xFF, 0xC5, 0xDE, 0x78, 0xFF,
  0xBD, 0xD6, 0x70, 0xFF, 0xC9, 0xE2, 0x7C, 0xFF, 0xC6, 0xDF, 0x79, 0xFF, 0xC5, 0xDE, 0x78, 0xFF,
  0xBD, 0xD6, 0x71, 0xFF, 0x9E, 0xB6, 0x54, 0xFF, 0x67, 0x7E, 0x25, 0xFF, 0x5D, 0x73, 0x1E, 0xFF,
  0xC1, 0xD9, 0x74, 0xFF, 0xAC, 0xC4, 0x60, 0xFF, 0x79, 0x90, 0x33, 0xFF, 0x5D, 0x73, 0x1E, 0xFF,
  0x5E, 0x74, 0x1F, 0xFF, 0x5B, 0x71, 0x1D, 0xFF, 0x5A, 0x71, 0x1D, 0xFF, 0x58, 0x6E, 0x1B, 0xFF,
  0x5E, 0x75, 0x1F, 0xFF, 0x5D, 0x74, 0x1E, 0xFF, 0x5A, 0x70, 0x1C, 0xFF, 0x5A, 0x70, 0x1C, 0xFF,
  0x54, 0x68, 0x19, 0xFF, 0x52, 0x67, 0x18, 0xFF, 0x4F, 0x62, 0x18, 0xFF, 0x4B, 0x5D, 0x16, 0xFF,
  0x57, 0x6C, 0x1A, 0xFF, 0x53, 0x67, 0x19, 0xFF, 0x4E, 0x62, 0x17, 0xFF, 0x4A, 0x5C, 0x16, 0xFF,
  0x46, 0x56, 0x15, 0xFF, 0x43, 0x53, 0x14, 0xFF, 0x30, 0x3A, 0x11, 0xD6, 0x1B, 0x1B, 0x1B, 0xFF,
  0x47, 0x58, 0x15, 0xFF, 0x43, 0x54, 0x14, 0xFF, 0x2F, 0x39, 0x11, 0xD6, 0x1B, 0x1B, 0x1B, 0x57,
  0x30, 0x30, 0x30, 0x32, 0x3B, 0x3B, 0x3B, 0x17, 0x42, 0x42, 0x42, 0x06, 0x47, 0x47, 0x47, 0x55,
  0x31, 0x31, 0x31, 0x31, 0x3B, 0x3B, 0x3B, 0x16, 0x43, 0x43, 0x43, 0x06, 0x47, 0x47, 0x47, 0x00,
  0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0x00,
  0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0x00, 0x49, 0x49, 0x49, 0xFF,
};

std::atomic<bool> g_a_pixel_swap_attempted{false};

// DISPROVEN (2026-09-12, fable2_052.log round / "Round 8"): DIAG4 scanned the
// full 8MB safely-readable window around bnk_base_probe and found 0 matches
// for kAButtonX360PixelSig. The real A-button payload is therefore NOT
// present as a flat identical byte copy anywhere in that neighborhood -- per
// explicit user direction, do not widen this blind scan further. Disabled
// per standing constraint ("keep disproven experimental paths disabled").
// Superseded by the [DIAG5] pipeline trace added this round in the real
// ReXGlue runtime source (tools/rexglue-source/src/filesystem/devices/
// host_path_file.cpp and src/graphics/pipeline/texture/cache.cpp), which
// traces the actual file-read -> texture-cache path instead of scanning
// memory for a byte pattern. See AI_HANDOFF.md "Round 8" section. Left in
// place (code untouched otherwise) so it can be re-enabled/retargeted
// immediately once [DIAG5] yields a confirmed guest-physical address.
constexpr bool kEnableExperimentalAPixelSwap = false;
constexpr size_t kPixelSwapScanWindowBytes = 8 * 1024 * 1024;  // 8MB -- wider than kBnkScanWindowBytes since gui_textures.bnk content, if resident at all, is not confirmed to be co-located with gui_texture_headers.bnk the way the name-hash was.

void TryExperimentalAPixelSwap(uint8_t* base, uint32_t bnk_base_probe) {
  if constexpr (!kEnableExperimentalAPixelSwap) return;
  if (bnk_base_probe == 0 || base == nullptr) return;
  if (g_a_pixel_swap_attempted.exchange(true, std::memory_order_acq_rel)) return;  // one-shot per process

  std::thread([base, bnk_base_probe]() {
    REXLOG_INFO("[GlyphSystem] [DIAG4] A-button real pixel-payload search starting on a worker thread (bnk_base=0x{:08X}, {}-byte window, {}-byte signature)...",
                bnk_base_probe, kPixelSwapScanWindowBytes, sizeof(kAButtonX360PixelSig));

    size_t safe_bytes = ComputeSafeReadableWindowBytes(base, bnk_base_probe, kPixelSwapScanWindowBytes);
    REXLOG_INFO("[GlyphSystem] [DIAG4] Safely-readable window: {} of {} requested bytes (bnk_base=0x{:08X}).",
                safe_bytes, kPixelSwapScanWindowBytes, bnk_base_probe);

    const uint8_t* host_base_ptr = GetHostReadPtr(base, bnk_base_probe);
    if (!host_base_ptr) host_base_ptr = base + bnk_base_probe;

    int matches_found = 0;
    constexpr int kMaxMatchesLogged = 8;
    constexpr size_t kSigWindowNeeded = kAButtonPixelSigRelOffset + sizeof(kAButtonX360PixelSig);
    for (size_t off = 0; off + kSigWindowNeeded <= safe_bytes; off += 4) {
      if (std::memcmp(host_base_ptr + off + kAButtonPixelSigRelOffset, kAButtonX360PixelSig, sizeof(kAButtonX360PixelSig)) != 0) continue;

      uint32_t tile_guest_addr = bnk_base_probe + static_cast<uint32_t>(off);
      ++matches_found;
      if (matches_found > kMaxMatchesLogged) break;
      REXLOG_INFO("[GlyphSystem] [DIAG4] ** SIGNATURE MATCH #{} ** candidate A pixel tile at guest_addr=0x{:08X} (offset 0x{:08X} from bnk_base)",
                  matches_found, tile_guest_addr, off);

      uint32_t candidate_tile_start = tile_guest_addr - static_cast<uint32_t>(kAButtonPixelSigRelOffset);
      if (!IsValidGuestReadPtr(base, candidate_tile_start, kAButtonPixelPayloadSize) ||
          !IsValidGuestWritePtr(base, candidate_tile_start, kAButtonPixelPayloadSize)) {
        REXLOG_WARN("[GlyphSystem] [DIAG4]   candidate #{} tile_start=0x{:08X}: full {}-byte region not both readable AND writable -- skipped.",
                    matches_found, candidate_tile_start, kAButtonPixelPayloadSize);
        continue;
      }

      // g_glyph_mutex: GetGlyphPixels() reads/writes the shared g_glyph_cache
      // map, which UpdateAllSlots()/OnResourceResolved's legacy path also
      // touch from the main thread under this same lock (see its
      // declaration above) -- this function runs on its own background
      // thread, so it must take the same lock rather than assume
      // exclusive access.
      std::vector<uint8_t> tiled_buf(kAButtonPixelPayloadSize);
      {
        std::lock_guard<std::mutex> glyph_lock(g_glyph_mutex);
        const auto& pixels = GetGlyphPixels("space", 64, 4);
        if (pixels.size() < kAButtonPixelPayloadSize) {
          REXLOG_WARN("[GlyphSystem] [DIAG4]   candidate #{} tile_start=0x{:08X}: 'space' glyph asset unavailable/too small ({} < {} bytes) -- cannot write. Check pc/assets/glyphs/space_64.bin is deployed next to the built exe (see GetGameRootDir()/LoadBinaryAsset() above).",
                      matches_found, candidate_tile_start, pixels.size(), kAButtonPixelPayloadSize);
          continue;
        }
        TilePixels(pixels.data(), tiled_buf.data(), 64, 64, 4);
      }

      std::vector<uint8_t> before_buf(kAButtonPixelPayloadSize);
      ReadGuestPixels(base, candidate_tile_start, before_buf.data(), kAButtonPixelPayloadSize);
      uint32_t hash_before = Fnv1aHash32(before_buf.data(), kAButtonPixelPayloadSize);
      uint32_t hash_expected_after = Fnv1aHash32(tiled_buf.data(), kAButtonPixelPayloadSize);

      WriteGuestPixels(base, candidate_tile_start, tiled_buf.data(), kAButtonPixelPayloadSize);

      std::vector<uint8_t> verify_buf(kAButtonPixelPayloadSize);
      ReadGuestPixels(base, candidate_tile_start, verify_buf.data(), kAButtonPixelPayloadSize);
      uint32_t hash_actual_after = Fnv1aHash32(verify_buf.data(), kAButtonPixelPayloadSize);
      REXLOG_INFO("[GlyphSystem] [DIAG4]   ** WROTE ** candidate #{} tile_start=0x{:08X}: replaced {} bytes with the tiled 'space' glyph. hash_before=0x{:08X} hash_expected_after=0x{:08X} hash_actual_after=0x{:08X} verify={}",
                  matches_found, candidate_tile_start, kAButtonPixelPayloadSize, hash_before, hash_expected_after, hash_actual_after,
                  hash_actual_after == hash_expected_after ? "OK" : "MISMATCH");
    }

    if (matches_found == 0) {
      REXLOG_INFO("[GlyphSystem] [DIAG4] No signature match found in {} scanned bytes from bnk_base=0x{:08X} -- A's pixel tile is not co-located with the header bank the way the name-hash was (or is not resident as a flat 16KB copy at all). See glyph-vfs-redirect-status.md for the next-round plan (widen the search / find the real consumer of the header pointer instead).",
                  safe_bytes, bnk_base_probe);
    }
    REXLOG_INFO("[GlyphSystem] [DIAG4] A-button pixel-payload search complete: {} signature match(es), {} scanned bytes from bnk_base=0x{:08X}.",
                matches_found, safe_bytes, bnk_base_probe);
  }).detach();
}

// [DIAG5] HISTORICAL NOTE (2026-09-12). Two earlier rounds ("Round 9"/
// fable2_053.log, then "link-fix round") added a bridge function here
// (`Fable2Glyph_TryGetTiledSpaceGlyphBytesForA`, first `extern "C"`, then
// registered with a generic `rex::Runtime` callback slot) so
// tools/rexglue-source/src/graphics/pipeline/texture/cache.cpp's GPU-side
// texture cache could reach this file's tiled "space" glyph bytes across
// the binary boundary (cache.cpp compiles into rexgpu-xenos.dll, this file
// into the host executable). BOTH attempts failed to even compile/link,
// because `native/build-gpu.ps1` builds rexgpu-xenos.dll as a standalone
// project (`native/gpu/CMakeLists.txt`) against the PREBUILT
// tools/rexglue-sdk/win-amd64 package -- confirmed via that build's own
// CMake configure output ("Found ReXGlue SDK 0.10.0 at .../tools/
// rexglue-sdk/win-amd64") -- so cache.cpp never even sees edits made to
// tools/rexglue-source/include or src/system (including any bridge added to
// `rex::Runtime`); only tools/rexglue-source/src/graphics itself is ever
// freshly compiled for that target. The SAME is true of the main
// `fable2.exe` build (`native/build.log` logs the identical
// "Found ReXGlue SDK..." line for the `win-amd64-release` preset) -- neither
// binary in this project is ever built from tools/rexglue-source/src/system.
//
// The fix (GPU-local-payload round) removed the cross-binary bridge
// entirely: cache.cpp now embeds the tiled "space" glyph's 16384 bytes
// directly as `kDiagTiledSpaceGlyphBytes`, precomputed once, offline, from
// this file's own GetGlyphPixels()/TilePixels() output on
// pc/assets/glyphs/space_64.bin -- so cache.cpp needs nothing from this
// file or this binary at all. This function and its registration machinery
// are gone because nothing calls them anymore; GetGlyphPixels()/
// TilePixels() themselves are unchanged and still used everywhere else in
// this file (button glyph rendering, the disabled DIAG4 experiment, etc.).
// See tools/rexglue-source/src/graphics/pipeline/texture/cache.cpp's
// top-of-file comment and AI_HANDOFF.md's "GPU-local-payload round" section
// for the full account.

// EXPERIMENTAL, DISPROVEN (2026-09-12, fable2_045.log round). See kEnableExperimentalAWrite
// for the full rationale. Called from inside OnResourceResolved's existing
// capped [DIAG2] diagnostic block, ONLY when btn_type == ButtonType::kA and
// only when kEnableExperimentalAWrite is true. Strictly guarded:
//   1. No-ops immediately if we've already written (g_exp_a_written) -- the
//      write is one-shot per KBM "session" (UpdateAllSlots() clears the
//      latch and restores original bytes on switch-away-from-KBM, allowing
//      a fresh validate-and-write the next time KBM becomes active again).
//   2. Re-validates, every call, that resource_guest_ptr is both readable
//      AND writable (IsValidGuestReadPtr / IsValidGuestWritePtr, NOT just
//      the 32-byte peek the surrounding diagnostic already did) for the
//      FULL expected buffer size for this button's known dimensions.
//   3. Samples several detiled (x,y) pixels via the existing
//      GetTiledOffset2D() math and logs them -- both so a human reviewing
//      the log can sanity-check plausibility, and so the plausibility
//      heuristic in step 4 has concrete numbers behind it.
//   4. Refuses to write if the buffer looks degenerate (fewer than a
//      handful of distinct byte values across the whole buffer -- e.g. an
//      all-zero or all-0xFF region, which would indicate we are NOT
//      actually looking at real pixel content despite passing the
//      read/write checks).
//   5. Only if 1-4 all pass: backs up the full original buffer, resolves
//      the KBM replacement glyph via the existing
//      ResolvePcGlyphForButton()/GetGlyphPixels() pipeline, tiles it with
//      the existing TilePixels() (identical logic the disabled legacy path
//      and UpdateAllSlots() both already use for every other button), and
//      writes it via the existing WriteGuestPixels() -- the same guest-
//      memory write mechanism already relied on elsewhere in this file to
//      reach the GPU's committed texture memory / trigger Xenia's MMIO-
//      handler-driven re-upload path (see rex/system/mmio_handler.h).
// Never mutates g_slots, never runs for any button but A, and never runs at
// all unless kEnableExperimentalAWrite is true.
void TryExperimentalAWriteForA(const char* name, uint32_t resource_guest_ptr,
                                const ButtonBnkDef* def, uint8_t* base) {
  if constexpr (!kEnableExperimentalAWrite) return;
  if (!def || def->type != ButtonType::kA) return;

  std::lock_guard<std::mutex> lock(g_glyph_mutex);
  if (g_exp_a_written) return; // one-shot until UpdateAllSlots() restores+clears

  const size_t buffer_size = static_cast<size_t>(def->width) * def->height * def->bpp;

  if (!IsValidGuestReadPtr(base, resource_guest_ptr, buffer_size) ||
      !IsValidGuestWritePtr(base, resource_guest_ptr, buffer_size)) {
    REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE] '{}' resource_guest_ptr=0x{:08X} NOT valid read+write for full buffer_size={} -- skipping",
                name, resource_guest_ptr, buffer_size);
    return;
  }

  std::vector<uint8_t> full_buf(buffer_size);
  ReadGuestPixels(base, resource_guest_ptr, full_buf.data(), buffer_size);

  // Step 3: sample a handful of detiled (x,y) pixels and log them.
  const uint32_t bpb_log2 = (def->bpp == 4) ? 2 : 0;
  const int32_t sample_coords[5][2] = {{0, 0}, {static_cast<int32_t>(def->width) - 1, 0},
                                        {0, static_cast<int32_t>(def->height) - 1},
                                        {static_cast<int32_t>(def->width) - 1, static_cast<int32_t>(def->height) - 1},
                                        {static_cast<int32_t>(def->width) / 2, static_cast<int32_t>(def->height) / 2}};
  for (const auto& c : sample_coords) {
    int32_t off = def->tiled ? GetTiledOffset2D(c[0], c[1], def->width, bpb_log2)
                              : static_cast<int32_t>((c[1] * def->width + c[0]) * def->bpp);
    if (off < 0 || static_cast<size_t>(off) + def->bpp > buffer_size) continue;
    if (def->bpp == 4) {
      REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE]   detiled px ({:2d},{:2d}) @ off {:5d}: RGBA=0x{:02X}{:02X}{:02X}{:02X}",
                  c[0], c[1], off, full_buf[off], full_buf[off + 1], full_buf[off + 2], full_buf[off + 3]);
    } else {
      REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE]   detiled px ({:2d},{:2d}) @ off {:5d}: A=0x{:02X}",
                  c[0], c[1], off, full_buf[off]);
    }
  }

  // Step 4: degeneracy check -- refuse to write over something that doesn't
  // look like real image data (too little variation across the buffer).
  std::unordered_set<uint8_t> distinct_bytes(full_buf.begin(), full_buf.end());
  if (distinct_bytes.size() < 8) {
    REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE] '{}' buffer at 0x{:08X} looks degenerate (only {} distinct byte value(s) across {} bytes) -- refusing to write",
                name, resource_guest_ptr, distinct_bytes.size(), buffer_size);
    return;
  }

  // Step 5: high confidence reached -- back up, resolve replacement glyph,
  // tile, and write.
  std::string glyph_name = ResolvePcGlyphForButton(ButtonType::kA);
  const auto& pixels = GetGlyphPixels(glyph_name, def->width, def->bpp);
  if (pixels.size() < buffer_size) {
    REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE] '{}' replacement glyph '{}' too small ({} < {}) -- refusing to write",
                name, glyph_name, pixels.size(), buffer_size);
    return;
  }

  uint32_t hash_before = Fnv1aHash32(full_buf.data(), buffer_size);

  g_exp_a_backup = std::move(full_buf); // already holds the pre-write original bytes
  g_exp_a_guest_addr = resource_guest_ptr;
  g_exp_a_glyph_name = glyph_name;

  std::vector<uint8_t> write_buf(buffer_size);
  if (def->tiled) {
    TilePixels(pixels.data(), write_buf.data(), def->width, def->height, def->bpp);
  } else {
    std::memcpy(write_buf.data(), pixels.data(), buffer_size);
  }
  uint32_t hash_expected_after = Fnv1aHash32(write_buf.data(), buffer_size);
  WriteGuestPixels(base, resource_guest_ptr, write_buf.data(), buffer_size);
  g_exp_a_written = true;

  // Read the guest memory BACK (not just trust the WriteGuestPixels call
  // returned) and hash it, so the log alone proves the write actually
  // landed rather than merely being attempted.
  std::vector<uint8_t> verify_buf(buffer_size);
  ReadGuestPixels(base, resource_guest_ptr, verify_buf.data(), buffer_size);
  uint32_t hash_actual_after = Fnv1aHash32(verify_buf.data(), buffer_size);
  bool write_verified = (hash_actual_after == hash_expected_after);

  REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE] *** WROTE experimental KBM glyph '{}' directly to resource_guest_ptr 0x{:08X} ({} bytes, {}x{} {}bpp, tiled={}) ***",
              glyph_name, resource_guest_ptr, buffer_size, def->width, def->height, def->bpp, def->tiled);
  REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE]   hash_before=0x{:08X} hash_expected_after=0x{:08X} hash_actual_after(read back)=0x{:08X} write_verified={}",
              hash_before, hash_expected_after, hash_actual_after, write_verified);
  if (!write_verified) {
    REXLOG_WARN("[GlyphSystem] [EXP-A-WRITE]   *** READBACK MISMATCH -- guest memory at 0x{:08X} does NOT contain the bytes we just wrote (something else overwrote it between our write and our read-back, or the write did not actually land) ***",
                resource_guest_ptr);
  }
  REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE]   Original bytes backed up in-memory ({} bytes, hash_before=0x{:08X}); will auto-restore via UpdateAllSlots() on next switch away from KBM.",
              g_exp_a_backup.size(), hash_before);
  REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE]   DECISIVE TEST: if the pause/menu A glyph now shows the KBM key art, this address is confirmed as the real sampled surface for BOTH title/world and menu/card A. If title changes but menu does not, the menu samples a separate GPU surface despite sharing the same logical header.");
}

void OnResourceResolved(uint32_t name_guest_ptr, uint32_t resource_guest_ptr, uint8_t* base) {
  g_ppc_base = base;

  // Sync the VFS bank redirect on every gui resource resolution, not just
  // controller-button ones: this is the most reliable point we have to catch
  // the VFS coming up (see the comment on UpdateBankRedirect), and it is
  // cheap to call repeatedly since it no-ops once the target already matches.
  UpdateBankRedirect(IsKbmActive());

  if (name_guest_ptr < 0x10000 || resource_guest_ptr < 0x10000 || base == nullptr) return;

  // Safe string length check (max 256 characters)
  size_t len = 0;
  while (len < 256) {
    char c = *(base + name_guest_ptr + len);
    if (c == '\0') break;
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) return;
    ++len;
  }
  if (len == 0 || len >= 256) return;

  const char* name = reinterpret_cast<const char*>(base + name_guest_ptr);

  // DIAGNOSTIC (2026-09-11, revised): log up to the first 5 occurrences of
  // each distinct GUI resource name seen here (not just IdentifyButton()-
  // recognized ones), each with its resource_guest_ptr. A prior version of
  // this tap deduplicated down to *one* log line per distinct name ever --
  // which meant a second resolve call for the exact same name string (e.g.
  // the pause menu re-requesting "icon_button_a.tex" after the title screen
  // already did) would be silently swallowed, making "the menu never
  // resolves this resource" and "the menu resolves it again but we only
  // logged it once" indistinguishable. Confirmed via user testing: the
  // pause/start menu's A-glyph IS the same logical prompt as the working
  // title-screen one, and only 'icon_button_a.tex' ever showed up in a
  // capture that included opening the menu -- so a repeat call for that
  // exact name is the specific thing to check for now. Logging
  // resource_guest_ptr on each occurrence lets us tell apart: (a) the menu
  // never re-resolves this name at all (only 1 occurrence logged even
  // though the menu was open) -- points to a cached/reused texture object
  // or a rendering path that bypasses this hook entirely for menu draws;
  // (b) a second occurrence with a DIFFERENT resource_guest_ptr -- proves a
  // second, distinct resource instance exists (a second copy/variant of the
  // icon, possibly in a different bank/atlas, that the VFS redirect/repack
  // never touched); (c) a second occurrence with the SAME resource_guest_ptr
  // -- true cache reuse, meaning the swap should already be visually
  // consistent, and the real cause lies further downstream (e.g. a GPU-side
  // texture uploaded once and never re-read from the bank). Purely
  // additive: does not touch UpdateBankRedirect or the
  // kEnableLegacyMemoryPatch-gated logic below. Remove once the menu glyph
  // investigation concludes -- see glyph-vfs-redirect-status.md.
  {
    static std::mutex s_trace_mutex;
    static std::unordered_map<std::string, int> s_name_occurrences;
    std::lock_guard<std::mutex> trace_lock(s_trace_mutex);
    auto it = s_name_occurrences.find(name);
    if (it == s_name_occurrences.end() && s_name_occurrences.size() < 4000) {
      it = s_name_occurrences.emplace(name, 0).first;
    }
    if (it != s_name_occurrences.end() && it->second < 4000) {
      ++it->second;
      REXLOG_INFO("[GlyphSystem] [ResourceTrace] name='{}' occurrence=#{} name_ptr=0x{:08X} res_id=0x{:08X}",
                  name, it->second, name_guest_ptr, resource_guest_ptr);
    }
  }

  ButtonType btn_type = IdentifyButton(name);
  if (btn_type == ButtonType::kUnknown) return;

  // TARGETED LOGGING: trace A glyph texture path for menu investigation
  if (btn_type == ButtonType::kA) {
    static std::mutex trace_mutex;
    static std::string last_binding;
    const auto binding = GetCurrentBinding(GameAction::kA);
    const auto primary = BindingToString(binding);
    std::lock_guard<std::mutex> trace_lock(trace_mutex);
    if (primary != last_binding) {
      last_binding = primary;
      REXLOG_INFO("[InteractGlyph] world resolve: primary='{}' glyph='{}' resolver='{}' resource=0x{:08X} legacy_enabled={} (active pixels come from bank)",
                  primary, GlyphNameForBinding(binding), ResolvePcGlyphForButton(ButtonType::kA),
                  resource_guest_ptr, kEnableLegacyMemoryPatch);
    }
    REXLOG_INFO("[GlyphSystem] [MENU-GLYPH-TRACE] A glyph resolve: name='{}' res_id=0x{:08X} kbm_active={} vfs_redirect_kbm={}",
                name, resource_guest_ptr, IsKbmActive(), g_bank_redirect_is_kbm.load(std::memory_order_acquire));
  }

  // DIAGNOSTIC (2026-09-12, unconditional -- independent of
  // kEnableLegacyMemoryPatch, and strictly read-only): everything from
  // LocateBnkBaseInMemory() through the GPU-physical-address extraction
  // below this point is currently DEAD CODE, because kEnableLegacyMemoryPatch
  // is false and the `if constexpr` block immediately after this one returns
  // unconditionally before any of it runs. That means every "res_id=..."
  // correlation made in this investigation so far reflects only the
  // ResourceTrace log above -- a resource-table lookup -- and NEVER actual
  // GPU/pixel-buffer state, since that code has not executed in any capture
  // taken with the VFS-redirect mechanism as the active fix. This block
  // re-enables a read-only slice of it -- no WriteGuestPixels, no g_slots
  // mutation, no g_glyph_mutex -- purely to observe, per icon_button_*
  // resolve: (a) whether resource_guest_ptr's own memory looks like a
  // decoded texture/header structure directly, independent of the whole-
  // bank scan LocateBnkBaseInMemory performs, and (b) whether
  // LocateBnkBaseInMemory still returns 0 on every call now that the bank is
  // streamed on demand (confirming, rather than assuming, the legacy path's
  // premise is dead in the current build). Tagged by a per-button-type
  // occurrence counter (separate from s_name_occurrences above) so a
  // title-screen resolve and a later pause-menu resolve of the exact same
  // name can be told apart by call order in the log. See
  // glyph-vfs-redirect-status.md (the round following fable2_043.log) for
  // the reasoning this supports. Purely additive: does not touch
  // UpdateBankRedirect, g_slots, or any guest-memory write.
  {
    static std::mutex s_gpu_diag_mutex;
    // Raised from 40 -> 4000 (fable2_045.log round): the 40-cap was proven
    // to be OUR OWN instrumentation truncating visibility, not a natural
    // end of activity -- the separate, higher-cap (150) "sub_82817748
    // resolve #N" diagnostic showed icon_button_a.tex resolves continuing
    // to occurrence #126 in the same capture, well past where this cap cut
    // us off. 4000 comfortably covers a multi-minute session revisiting
    // title/world AND the pause menu without truncating.
    static std::unordered_map<int, int> s_button_occurrences; // ButtonType -> count
    std::lock_guard<std::mutex> gpu_diag_lock(s_gpu_diag_mutex);
    int type_key = static_cast<int>(btn_type);
    int& occ = s_button_occurrences[type_key];
    if (occ < 4000) {
      ++occ;

      // Look up this button's known bank definition (dimensions/bpp/tiled)
      // by btn_type alone -- independent of bnk_base_probe below. Previously
      // this lookup only happened inside the `bnk_base_probe != 0` branch
      // (for reading the static header), which meant it was unavailable for
      // validating resource_guest_ptr itself. Hoisted here so both uses
      // share one lookup.
      const ButtonBnkDef* found_def = nullptr;
      for (const auto& d : kButtonBnkDefs) {
        if (d.type == btn_type) { found_def = &d; break; }
      }

      uint32_t bnk_base_probe = LocateBnkBaseInMemory(base);
      REXLOG_INFO("[GlyphSystem] [DIAG2] '{}' btn_occurrence=#{} res_id=0x{:08X} bnk_base_probe=0x{:08X}",
                  name, occ, resource_guest_ptr, bnk_base_probe);

      // DIAGNOSTIC (2026-09-12, fable2_047.log round): cross-check that
      // resource_guest_ptr (the misleadingly-named legacy parameter this
      // whole file has carried since round 1) really is
      // GameNameHash32(name) as this round's trace/verification concluded,
      // rather than trusting the offline Python re-derivation alone. Logs a
      // clear, unambiguous confirmation or mismatch line per resolve so
      // every future capture stands on its own as proof, without requiring
      // a reader to cross-reference this file's history. A mismatch here
      // would be a serious signal that the traced algorithm is wrong (or
      // name-normalization/casing differs from what GameNameHash32 expects)
      // and would need immediate re-investigation before trusting anything
      // built on top of this conclusion.
      {
        uint32_t computed_hash = GameNameHash32(name);
        if (computed_hash == resource_guest_ptr) {
          REXLOG_INFO("[GlyphSystem] [DIAG2]   [HASH CONFIRMED] GameNameHash32('{}') == 0x{:08X} == resource_guest_ptr -- this parameter is a NAME HASH, not a pixel-buffer pointer.",
                      name, computed_hash);
        } else {
          REXLOG_WARN("[GlyphSystem] [DIAG2]   [HASH MISMATCH] GameNameHash32('{}') = 0x{:08X} != resource_guest_ptr = 0x{:08X} -- name-hash theory does NOT hold for this resolve; needs re-investigation.",
                      name, computed_hash, resource_guest_ptr);
        }
      }

      // PERFORMANCE FIX (2026-09-13, fable2_050.log round): round 5's
      // per-hash, synchronous, game-thread-blocking ScanBnkForNameHash()
      // calls here (one for resource_guest_ptr, then one MORE per entry in
      // kKnownResourceHashProbes) caused the fable2_050.log boot stall --
      // each full ~1MB linear scan cost ~11-16 seconds, and they ran back
      // to back on the same thread that OnResourceResolved() itself runs
      // on (there is no separate diagnostics thread in this codebase), so
      // a single icon resolve during boot could block the game for 30+
      // seconds. Confirmed via fable2_050.log timestamps: A's scan took
      // ~11.06s, DPad's ~15.85s, and B's scan was still running (a match
      // was found and logged, but no "Scan complete" line) when the user
      // closed the game -- X and Y never even started.
      //
      // Replaced with a single call to RunBankHashScanBackground(), which
      // launches (at most once ever, guarded by g_bank_scan_launched) one
      // background std::thread that does ONE linear pass over the bank,
      // checking every word against an O(1) hash-set lookup covering ALL
      // known hashes in kKnownResourceHashProbes simultaneously (A is
      // already the first entry, so resource_guest_ptr's own hash doesn't
      // need a separate call). AnalyzeBnkMatchContext() still runs, once
      // per actual match, exactly as in round 5 -- it was never the
      // bottleneck. This call is non-blocking and returns immediately on
      // every invocation after the first.
      RunBankHashScanBackground(base, bnk_base_probe);

      // TODAY'S PRIMARY OBJECTIVE (2026-09-12, fable2_051.log round): search
      // for A's REAL, currently-resident 64x64 RGBA8 pixel tile (not just
      // its 4-byte name hash) and, if a fully-validated match against the
      // stale ORIGINAL/X360 bytes is found, overwrite it in place with the
      // tiled "space" PC glyph. One-shot per process; see
      // TryExperimentalAPixelSwap()'s own extensive doc comment above for
      // the full derivation of kAButtonPixelFileOffset and the signature.
      TryExperimentalAPixelSwap(base, bnk_base_probe);

      // RETIRED (2026-09-13, fable2_048.log round): this block used to dump
      // resource_guest_ptr's OWN memory contents ("resource_ptr words 0..7"),
      // flag "candidate GPU-physical-looking" words found within it, and run
      // a full-buffer read/write-ability check treating resource_guest_ptr
      // itself as a pixel-buffer address. Round 4 (fable2_047.log)
      // conclusively proved resource_guest_ptr is GameNameHash32(name) -- a
      // 32-bit name hash, not a guest pointer at all (see GameNameHash32()
      // and the [HASH CONFIRMED] cross-check above). Treating a hash value
      // as a dereferenceable address and inspecting whatever memory
      // coincidentally happened to live there was always a category error:
      // every "candidate" this block ever logged in earlier rounds was
      // necessarily coincidental heap content, not resource data. Removed
      // rather than left running, to avoid polluting future captures with
      // misleading candidates. RunBankHashScanBackground() (called above;
      // round 5's ScanBnkForNameHash() before it, since replaced for
      // performance -- see that function's fable2_050.log-round doc
      // comment) plus AnalyzeBnkMatchContext() are the real,
      // structurally-grounded successor to this thread: the hash is now
      // known to appear as a
      // literal value inside gui_texture_headers.bnk itself (bank offset
      // 0x1A8EC for icon_button_a.tex, confirmed fable2_048.log), and that
      // location -- not resource_guest_ptr's numeric value used as an
      // address -- is where the real lookup path is now being traced from.
      // See glyph-vfs-redirect-status.md's fable2_048.log-round section for
      // the full writeup.

      // EXPERIMENTAL, DISPROVEN (2026-09-12, fable2_045.log round): A-only guarded
      // write attempt -- see kEnableExperimentalAWrite and
      // TryExperimentalAWriteForA() for the full guard chain. No-op for
      // every button except A, and a no-op entirely if
      // kEnableExperimentalAWrite is false.
      if (btn_type == ButtonType::kA && found_def) {
        TryExperimentalAWriteForA(name, resource_guest_ptr, found_def, base);
      }

      // DIAGNOSTIC (2026-09-12, fable2_044.log round, still strictly read-only):
      // `fable2_044.log` proved `bnk_base_probe` above is NOT always 0 -- it
      // resolved via the existing multi-button (A+Y) cross-checked signature
      // scan in LocateBnkBaseInMemory(), meaning gui_texture_headers.bnk IS
      // resident and locatable in guest RAM at least some of the time,
      // contradicting this file's own header comment (which assumed it never
      // is, for the "streamed on demand" reason given there). Now that a real
      // base is in hand, read the actual per-button header the disabled
      // legacy path would have used (bnk_base + def->bnk_offset from
      // kButtonBnkDefs) and dump it read-only, plus run the same w[2]/w[9..14]
      // candidate pixel-buffer / GPU-physical-address checks the disabled
      // path performs -- purely to observe candidates, never to write.
      if (bnk_base_probe != 0) {
        // Reuses `found_def` hoisted above (looked up by btn_type alone) --
        // no need for a second kButtonBnkDefs scan here.
        uint32_t hdr_guest_addr = found_def ? (bnk_base_probe + found_def->bnk_offset) : 0;
        if (hdr_guest_addr != 0 && IsValidGuestReadPtr(base, hdr_guest_addr, 88)) {
          const uint32_t* hdr_raw = reinterpret_cast<const uint32_t*>(GetHostReadPtr(base, hdr_guest_addr));
          if (!hdr_raw) hdr_raw = reinterpret_cast<const uint32_t*>(base + hdr_guest_addr);
          uint32_t hw[22];
          for (int i = 0; i < 22; ++i) hw[i] = __builtin_bswap32(hdr_raw[i]);
          REXLOG_INFO("[GlyphSystem] [DIAG2]   header@0x{:08X} w[0..7]: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                      hdr_guest_addr, hw[0], hw[1], hw[2], hw[3], hw[4], hw[5], hw[6], hw[7]);
          REXLOG_INFO("[GlyphSystem] [DIAG2]   header w[8..14]: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                      hw[8], hw[9], hw[10], hw[11], hw[12], hw[13], hw[14]);
          REXLOG_INFO("[GlyphSystem] [DIAG2]   header w[15..21]: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                      hw[15], hw[16], hw[17], hw[18], hw[19], hw[20], hw[21]);
          if (hw[2] >= 0x10000 && IsValidGuestReadPtr(base, hw[2], 16)) {
            REXLOG_INFO("[GlyphSystem] [DIAG2]   candidate pixel buffer from header w[2]: 0x{:08X}", hw[2]);
          }
          // DIAGNOSTIC (2026-09-12, fable2_044.log round, still read-only):
          // the crude "any word whose top 20 bits look address-shaped" scan is
          // replaced here by a real decode against
          // tools/rexglue-sdk/.../rex/graphics/xenos.h's `xe_gpu_texture_fetch_t`
          // (confirmed present in this SDK -- ReXGlue is a Xenia-derived
          // runtime with a real Xenos GPU command processor and D3D12/Vulkan
          // texture cache, per graphics/pipeline/texture/cache.h and
          // system/mmio_handler.h, both confirmed to exist this round). A real
          // fetch constant is 6 dwords; dword_1 bits 12..31 hold base_address
          // (<<12 for the byte address), dword_0 bit 31 holds `tiled`, and
          // dword_2 bits 0..12/13..25 hold (width-1)/(height-1) for a 2D
          // texture. Scan every 6-word window in the header for a candidate
          // whose decoded (width-1, height-1, tiled) matches this button's own
          // known dimensions (`kButtonBnkDefs`) exactly -- a match this
          // specific is very unlikely to be coincidental, unlike a bare
          // address-range heuristic.
          if (found_def) {
            for (int s = 0; s + 6 <= 22; ++s) {
              uint32_t d0 = hw[s], d1 = hw[s + 1], d2 = hw[s + 2];
              uint32_t base_addr = (d1 & 0xFFFFF000);
              bool tiled = (d0 >> 31) & 1;
              uint32_t w_minus1 = d2 & 0x1FFF;
              uint32_t h_minus1 = (d2 >> 13) & 0x1FFF;
              if (w_minus1 + 1 == found_def->width && h_minus1 + 1 == found_def->height &&
                  tiled == found_def->tiled && base_addr >= 0x10000) {
                REXLOG_INFO("[GlyphSystem] [DIAG2]   ** xe_gpu_texture_fetch_t MATCH at header word[{}]: base_address=0x{:08X} tiled={} w={} h={} (dword0=0x{:08X} dword1=0x{:08X} dword2=0x{:08X})",
                            s, base_addr, tiled, w_minus1 + 1, h_minus1 + 1, d0, d1, d2);
              }
            }
          }
        } else {
          REXLOG_INFO("[GlyphSystem] [DIAG2]   header address 0x{:08X} (bnk_base=0x{:08X}) not currently valid readable memory", hdr_guest_addr, bnk_base_probe);
        }
      }
    }
  }

  if constexpr (!kEnableLegacyMemoryPatch) {
    // Legacy RAM-injection path disabled; the VFS redirect above (synced
    // just before this function's guard checks) is the active mechanism.
    // Everything below this point -- LocateBnkBaseInMemory, the header/
    // pixel-buffer scan, and the direct guest-memory pixel overwrite -- is
    // kept only for reference.
    return;
  }

  std::lock_guard<std::mutex> lock(g_glyph_mutex);

  // Check if slot already exists for this button type
  for (auto& s : g_slots) {
    if (s.type == btn_type) {
      return; // Already registered
    }
  }

  // Find definition for this button type
  const ButtonBnkDef* def = nullptr;
  for (const auto& d : kButtonBnkDefs) {
    if (d.type == btn_type) {
      def = &d;
      break;
    }
  }
  if (!def) return;

  uint32_t bnk_base = LocateBnkBaseInMemory(base);
  if (bnk_base == 0) {
    static auto s_last_warn = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (now - s_last_warn >= std::chrono::seconds(2)) {
      s_last_warn = now;
      REXLOG_WARN("[GlyphSystem] [DIAG] gui_texture_headers.bnk not yet located in guest memory for '{}' (res_id=0x{:08X})",
                  name, resource_guest_ptr);
    }
    return;
  }

  uint32_t hdr_guest_addr = bnk_base + def->bnk_offset;
  if (!IsValidGuestReadPtr(base, hdr_guest_addr, 88)) {
    REXLOG_WARN("[GlyphSystem] [DIAG] Header guest address 0x{:08X} for '{}' is not valid readable memory",
                hdr_guest_addr, name);
    return;
  }

  const uint32_t* hdr_raw = reinterpret_cast<const uint32_t*>(GetHostReadPtr(base, hdr_guest_addr));
  if (!hdr_raw) hdr_raw = reinterpret_cast<const uint32_t*>(base + hdr_guest_addr);

  uint32_t w[22];
  for (int i = 0; i < 22; ++i) {
    w[i] = __builtin_bswap32(hdr_raw[i]);
  }
  REXLOG_INFO("[GlyphSystem] [DIAG] '{}' (res_id=0x{:08X}) header at 0x{:08X}:", name, resource_guest_ptr, hdr_guest_addr);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[0..3]:   0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}", w[0], w[1], w[2], w[3]);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[4..7]:   0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}", w[4], w[5], w[6], w[7]);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[8..11]:  0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}", w[8], w[9], w[10], w[11]);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[12..15]: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}", w[12], w[13], w[14], w[15]);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[16..19]: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}", w[16], w[17], w[18], w[19]);
  REXLOG_INFO("[GlyphSystem] [DIAG]   w[20..21]: 0x{:08X} 0x{:08X}", w[20], w[21]);

  if (IsValidGuestReadPtr(base, resource_guest_ptr, 32)) {
    const uint32_t* res_raw = reinterpret_cast<const uint32_t*>(GetHostReadPtr(base, resource_guest_ptr));
    if (!res_raw) res_raw = reinterpret_cast<const uint32_t*>(base + resource_guest_ptr);
    REXLOG_INFO("[GlyphSystem] [DIAG]   resource_ptr (0x{:08X}) words 0..7: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                resource_guest_ptr,
                __builtin_bswap32(res_raw[0]), __builtin_bswap32(res_raw[1]),
                __builtin_bswap32(res_raw[2]), __builtin_bswap32(res_raw[3]),
                __builtin_bswap32(res_raw[4]), __builtin_bswap32(res_raw[5]),
                __builtin_bswap32(res_raw[6]), __builtin_bswap32(res_raw[7]));
  }

  size_t buffer_size = def->width * def->height * def->bpp;

  // Identify where the actual pixel buffer is:
  uint32_t target_guest_addr = 0;
  bool is_initialized = false;

  if (w[2] >= 0x10000) {
    // If w[1] is buffer_size + 4, pixel data starts at offset +4
    uint32_t candidate = (w[1] == buffer_size + 4) ? (w[2] + 4) : w[2];
    if (IsValidGuestReadPtr(base, candidate, buffer_size)) {
      target_guest_addr = candidate;
      is_initialized = true;
      REXLOG_INFO("[GlyphSystem] [DIAG] Identified texture pixel buffer from header w[2]: 0x{:08X}", target_guest_addr);
    }
  }

  // Also check D3D texture fetch constant in words 9..14 for GPU physical base address
  // In Xenos TextureFetch dword_1, base_address is bits 12..31 (byte address = dword_1 & ~0xFFF)
  for (int idx = 9; idx <= 14; ++idx) {
    uint32_t cand_phys = w[idx] & 0xFFFFF000;
    if (cand_phys >= 0x10000 && cand_phys < 0x20000000) {
      REXLOG_INFO("[GlyphSystem] [DIAG] Candidate GPU physical address from w[{}]: 0x{:08X}", idx, cand_phys);
      if (target_guest_addr == 0) {
        uint32_t cand_guest = 0xA0000000 + cand_phys;
        if (IsValidGuestReadPtr(base, cand_guest, buffer_size)) {
          target_guest_addr = cand_guest;
          is_initialized = true;
        }
      }
    }
  }

  if (target_guest_addr == 0) {
    REXLOG_WARN("[GlyphSystem] [DIAG] Pixel buffer not yet resolved for '{}' (w[2]=0x{:08X})", name, w[2]);
    return;
  }

  Slot slot;
  slot.type = btn_type;
  slot.name = name;
  slot.resource_guest_ptr = resource_guest_ptr;
  slot.texture_guest_ptr = resource_guest_ptr;
  slot.base_address = target_guest_addr;
  slot.width = def->width;
  slot.height = def->height;
  slot.bpp = def->bpp;
  slot.tiled = def->tiled;
  slot.buffer_size = buffer_size;

  // Backup original Xbox pixels
  slot.original_xbox_pixels.resize(buffer_size);
  ReadGuestPixels(base, target_guest_addr, slot.original_xbox_pixels.data(), buffer_size);

  g_slots.push_back(std::move(slot));

  // Determine PC replacement glyph
  std::string glyph_name = ResolvePcGlyphForButton(btn_type);
  const auto& pixels = GetGlyphPixels(glyph_name, def->width, def->bpp);
  size_t bytes_written = 0;

  if (IsKbmActive() && pixels.size() >= buffer_size) {
    Slot& s = g_slots.back();
    if (s.tiled) {
      std::vector<uint8_t> tiled_buf(buffer_size);
      TilePixels(pixels.data(), tiled_buf.data(), s.width, s.height, s.bpp);
      WriteGuestPixels(base, target_guest_addr, tiled_buf.data(), buffer_size);
    } else {
      WriteGuestPixels(base, target_guest_addr, pixels.data(), buffer_size);
    }
    s.last_pc_glyph = glyph_name;
    bytes_written = buffer_size;
  }

  REXLOG_INFO("[GlyphSystem] [DIAG] First glyph replacement diagnostics:");
  REXLOG_INFO("[GlyphSystem] [DIAG]   glyph name: {} -> {}", name, glyph_name);
  REXLOG_INFO("[GlyphSystem] [DIAG]   target guest address: 0x{:08X}", target_guest_addr);
  REXLOG_INFO("[GlyphSystem] [DIAG]   expected buffer size: {}", buffer_size);
  REXLOG_INFO("[GlyphSystem] [DIAG]   replacement buffer size: {}", pixels.size());
  REXLOG_INFO("[GlyphSystem] [DIAG]   texture format/dimensions: {}x{}, {} bpp, {}",
              def->width, def->height, def->bpp, def->format_str);
  REXLOG_INFO("[GlyphSystem] [DIAG]   bytes written: {}", bytes_written);
  REXLOG_INFO("[GlyphSystem] [DIAG]   resource fully initialized: {}", is_initialized);
}

void UpdateAllSlots() {
  std::lock_guard<std::mutex> lock(g_glyph_mutex);
  REXLOG_INFO("[InteractGlyph] UpdateAllSlots: slots={} guest_base={} legacy_enabled={}",
              g_slots.size(), g_ppc_base != nullptr, kEnableLegacyMemoryPatch);
  if (!g_ppc_base) return;

  bool kbm = IsKbmActive();

  // Reverse the experimental A-button direct write (see
  // kEnableExperimentalAWrite / TryExperimentalAWriteForA) the same way the
  // loop below reverses the disabled legacy per-slot writes: on any
  // transition away from KBM, restore the original bytes we backed up
  // before writing, then clear the latch so a later switch back to KBM (and
  // a fresh icon_button_a.tex resolve) can re-arm and re-validate rather
  // than silently doing nothing forever.
  if (g_exp_a_written && !kbm) {
    WriteGuestPixels(g_ppc_base, g_exp_a_guest_addr, g_exp_a_backup.data(), g_exp_a_backup.size());
    REXLOG_INFO("[GlyphSystem] [EXP-A-WRITE] Restored original bytes at 0x{:08X} ({} bytes) -- input device switched away from KBM",
                g_exp_a_guest_addr, g_exp_a_backup.size());
    g_exp_a_written = false;
    g_exp_a_guest_addr = 0;
    g_exp_a_backup.clear();
  }

  if (g_slots.empty()) return;

  for (auto& slot : g_slots) {
    if (!kbm) {
      // Controller mode: restore original Xbox controller icon
      if (!slot.original_xbox_pixels.empty()) {
        WriteGuestPixels(g_ppc_base, slot.base_address, slot.original_xbox_pixels.data(), slot.buffer_size);
        slot.last_pc_glyph.clear();
      }
    } else {
      // KBM mode: resolve active PC binding glyph
      std::string glyph_name = ResolvePcGlyphForButton(slot.type);
      const auto& pixels = GetGlyphPixels(glyph_name, slot.width, slot.bpp);
      if (pixels.size() >= slot.buffer_size) {
        if (slot.tiled) {
          std::vector<uint8_t> tiled_buf(slot.buffer_size);
          TilePixels(pixels.data(), tiled_buf.data(), slot.width, slot.height, slot.bpp);
          WriteGuestPixels(g_ppc_base, slot.base_address, tiled_buf.data(), slot.buffer_size);
        } else {
          WriteGuestPixels(g_ppc_base, slot.base_address, pixels.data(), slot.buffer_size);
        }
        slot.last_pc_glyph = glyph_name;
      }
    }
  }
}

void TraceWorldInteractRead(uint32_t handle, uint64_t offset, uint32_t buffer,
                           uint32_t length) {
  // Verified against the live bank catalog: an I/O range, never a guest pointer.
  // Diagnostics only; no writes or cached addresses.
  constexpr uint64_t begin = 21026128;
  constexpr uint64_t end = begin + 16388;
  if (!length || offset >= end || offset + length <= begin) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  auto file = kernel->object_table()->LookupObject<rex::system::XFile>(handle);
  if (!file) return;
  const auto& path = file->entry()->absolute_path();
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) >= 16) return;
  REXLOG_INFO("[InteractGlyph] A bank read: path='{}' offset={} length={} buffer=0x{:08X} pixel_delta={} primary='{}'",
              path, offset, length, buffer, static_cast<int64_t>(begin + 4) - static_cast<int64_t>(offset),
              BindingToString(GetCurrentBinding(GameAction::kA)));
}

void TraceWorldInteractCopy(uint32_t destination, uint32_t source, uint32_t length,
                           uint32_t caller, uint8_t* base) {
  if (length != 16384 && length != 16388) return;
  const uint32_t skip = length == 16388 ? 4 : 0;
  if (!IsValidGuestReadPtr(base, source + skip, 16384)) return;
  static const auto bank_pixels = [] {
    auto rgba = LoadBinaryAsset("space_64.bin");
    if (rgba.size() != 16384) return std::vector<uint8_t>{};
    std::vector<uint8_t> tiled(rgba.size());
    TilePixels(rgba.data(), tiled.data(), 64, 64, 4);
    for (size_t i = 0; i < tiled.size(); i += 4)
      std::rotate(tiled.begin() + i, tiled.begin() + i + 3, tiled.begin() + i + 4);
    return tiled;
  }();
  if (bank_pixels.empty()) return;
  const auto* bytes = GetHostReadPtr(base, source + skip);
  if (!bytes || std::memcmp(bytes, bank_pixels.data(), bank_pixels.size()) != 0) return;

  // FIX (2026-09-14): this copy is confirmed -- by the exact 16KB content
  // match against the baked-SPACE reference above -- to be the world
  // Interact prompt's bank payload, one PPC instruction away from being
  // memcpy'd into whatever buffer the renderer actually samples. This is the
  // path that bypasses both OnResourceResolved()/g_slots (dead:
  // kEnableLegacyMemoryPatch/kEnableExperimentalAWrite are both false, so
  // g_slots is never populated and UpdateAllSlots() is a no-op) and the
  // art/gui KBM/X360 VFS directory redirect (UpdateBankRedirect() only
  // swaps which directory "art/gui" resolves to by name -- this payload is
  // read by raw file offset, never through a named resource lookup, so the
  // redirect never touches it). That is why rebinding keybind_a never
  // changed what the world prompt showed, even though
  // ResolvePcGlyphForButton()/GlyphNameForBinding() already resolve the
  // live binding correctly (verified: keybind_a="F" -> "f", "E,Space,Shift"
  // -> "e", both already-existing glyph assets).
  //
  // Substitute the SOURCE bytes, not destination: the plain memcpy body
  // immediately below this call (untouched, still generated code) copies
  // source -> destination right after we return, so patching destination
  // here would just be overwritten by the original bytes a few instructions
  // later. The replacement uses the EXACT same tile + per-pixel channel-
  // rotate transform verified above to reproduce this bank payload's
  // on-disk format byte-for-byte for SPACE -- both steps are required, or
  // the swapped glyph would render with the wrong tiling (garbled shape) or
  // wrong channel order (wrong colors). A resolve failure (unbound action,
  // key with no drawn glyph asset) or a missing/mis-sized glyph file leaves
  // `source` completely untouched, so the copy falls back to carrying the
  // original baked SPACE bytes through -- i.e. today's baseline behavior,
  // never a garbled write.
  std::string world_glyph_name = ResolvePcGlyphForButton(ButtonType::kA);
  if (!world_glyph_name.empty()) {
    static std::mutex s_world_glyph_cache_mutex;
    static std::unordered_map<std::string, std::vector<uint8_t>> s_world_glyph_cache;
    std::lock_guard<std::mutex> cache_lock(s_world_glyph_cache_mutex);
    auto cache_it = s_world_glyph_cache.find(world_glyph_name);
    if (cache_it == s_world_glyph_cache.end()) {
      std::vector<uint8_t> transformed;
      auto rgba = LoadBinaryAsset(world_glyph_name + "_64.bin");
      if (rgba.size() == 16384) {
        transformed.resize(16384);
        TilePixels(rgba.data(), transformed.data(), 64, 64, 4);
        for (size_t i = 0; i < transformed.size(); i += 4)
          std::rotate(transformed.begin() + i, transformed.begin() + i + 3, transformed.begin() + i + 4);
      }
      cache_it = s_world_glyph_cache.emplace(world_glyph_name, std::move(transformed)).first;
    }
    const auto& world_pixels = cache_it->second;
    if (world_pixels.size() == 16384 && IsValidGuestWritePtr(base, source + skip, 16384)) {
      WriteGuestPixels(base, source + skip, world_pixels.data(), world_pixels.size());
    }
  }

  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) >= 16) return;
  REXLOG_INFO("[InteractGlyph] exact A bank payload copy: src=0x{:08X} dst=0x{:08X} length={} caller=0x{:08X} substituted='{}'",
              source, destination, length, caller, world_glyph_name);
}

// NEW LEAD (2026-09-14): the memcpy-based trace above proved negative at
// runtime (zero [InteractGlyph] lines from either it or
// TraceWorldInteractRead in a full build+run capture), so this instruments
// the one OTHER mechanism this project has already confirmed actually
// drives an A-prompt SPACE substitution: sub_828152D0's Anark render-object
// creation path (CorrectAnarkRenderBinding(), gated on IsKbmActive(),
// called just before sub_82814BA8 builds the render object's draw
// descriptor -- see anark_render_bindings.h and the "green A" investigation
// history in glyph-vfs-redirect-status.md). That call site already logs a
// MATCH via "[GlyphSystem] [AnarkRenderBinding] ...", but only when `model`
// is one of kAnarkRenderBindings' 30 known entries -- so it stays silent,
// and gives no information at all, for any render-object this table
// doesn't cover, which is exactly the situation if the world Interact
// prompt uses a `model` id outside that table (or isn't an Anark
// render-object at all). This function logs UNCONDITIONALLY, every time
// sub_828152D0 runs while KBM is active, regardless of match -- so a
// capture that walks up to an in-world interactable and reads its prompt
// will show every distinct `model` id actually seen at that moment, proving
// (or disproving) that this call site is on the world prompt's real render
// path before any substitution logic is added here. Purely additive: does
// not read or write anything CorrectAnarkRenderBinding/kAnarkRenderBindings
// don't already read/write, and does not change control flow.
// Shared, mutex-protected "most recently seen resource name" snapshot.
// Populated unconditionally by TraceResourceLoadByName (the trace on
// sub_8222CF18, the shared "load resource by name" primitive confirmed to
// be the common choke-point every known scene loader funnels a resource
// name through -- see its own comment below) every time it captures a
// valid, printable name, independent of that function's own 150-occurrence
// logging cap. Read by TraceAnarkRenderObjectCreated as a best-available
// proxy for "which scene/screen is this render-object being created for,"
// since `model` ids are not globally stable across .bgf files (see the
// 2026-09-15 comment below). Diagnostic only -- nothing currently reads
// this to change control flow.
namespace {
std::mutex g_last_resource_name_mutex;
std::string g_last_resource_name_storage = "<none-yet>";
}  // namespace

static void SetLastResourceName(const char* name) {
  std::lock_guard<std::mutex> lock(g_last_resource_name_mutex);
  g_last_resource_name_storage = name;
}

static std::string GetLastResourceName() {
  std::lock_guard<std::mutex> lock(g_last_resource_name_mutex);
  return g_last_resource_name_storage;
}

void TraceAnarkRenderObjectCreated(uint32_t model, uint32_t hash_before) {
  static std::mutex s_mutex;
  static std::unordered_map<uint32_t, int> s_model_occurrences;
  std::lock_guard<std::mutex> lock(s_mutex);
  int& occ = s_model_occurrences[model];
  if (occ >= 20) return;
  ++occ;
  std::string last_name = GetLastResourceName();
  REXLOG_INFO("[InteractGlyph] [WORLD-ANARK-TRACE] sub_828152D0 render-object create: model={} hash_before=0x{:08X} occurrence=#{} last_resource=\"{}\"",
              model, hash_before, occ, last_name);
}

// DISABLED 2026-09-15 (same day as the comment below was originally
// written) -- the call site in sub_828152D0 that used this has been
// reverted. Matching by `model` id alone was wrong: 6702 ("rim_and_red")
// and 6723 ("KeyMarker") are STABLE, SHARED Anark component ids reused for
// the ordinary selected-row highlight badge on normal menu screens
// (Abilities, Skills, etc.) -- not unique to the world door "Unlock"
// hold-prompt this was meant to target. Suppressing the draw (early-return
// before this function's render-object setup) for those ids broke every
// other screen's highlight: the GPU-side render object was left
// never-(re)initialized, which rendered as a solid opaque blue rectangle
// over the selected-row highlight everywhere while KBM was active --
// confirmed live by the user immediately after this shipped, and reverted
// the same day. Left in place, disabled (always returns false), as a
// documented dead end: any future attempt at hiding rim_and_red/KeyMarker
// specifically for the world hold-prompt needs a way to distinguish that
// one call site/context from every other screen's use of the same shared
// ids -- `model` alone cannot do that. See glyph-vfs-redirect-status.md for
// the full history, including this regression and its revert.
bool ShouldSuppressAnarkRenderObjectForKbm(uint32_t model) {
  (void)model;
  return false;
}

// NEW LEAD (2026-09-14, second hop): sub_828152D0 (the menu selected-row FAC
// correction path) also proved negative at runtime -- zero
// [WORLD-ANARK-TRACE] lines while the world Interact prompt was visibly on
// screen. Offline FAC/BGF parsing (see the doc comment on the declaration in
// glyph_system.h) explains why: the world HUD's AButton node is compiled
// with the shared-atlas hash/UV already baked in, unconditionally, so it
// never goes through a per-bind runtime correction at all. sub_823FACE0 is
// the one concrete, evidence-backed (not guessed) function this project's
// own anark-scene-xrefs.json cross-reference ties to loading "HUD.bgf" by
// name. This traces its entry, unconditionally, every time it runs.
void TraceHudSceneLoad() {
  static std::atomic<unsigned> count{0};
  unsigned n = count.fetch_add(1);
  if (n >= 8) return;
  REXLOG_INFO("[InteractGlyph] [HUD-SCENE-TRACE] sub_823FACE0 (HUD.bgf loader) entered, kbm_active={} occurrence=#{}",
              IsKbmActive(), n + 1);
}

void TraceResourceLoadByName(uint32_t name_guest_ptr, uint8_t* base) {
  static std::atomic<unsigned> count{0};
  unsigned n = count.fetch_add(1);
  if (n >= 150) return;

  if (name_guest_ptr < 0x10000 || base == nullptr) {
    REXLOG_INFO("[InteractGlyph] [RESOURCE-LOAD-TRACE] sub_8222CF18 entered, name=<invalid ptr 0x{:08X}> occurrence=#{}",
                name_guest_ptr, n + 1);
    return;
  }

  // Safe string length check (max 256 characters) -- same idiom as
  // OnResourceResolved()'s existing guest-string read.
  size_t len = 0;
  while (len < 256) {
    char c = *(base + name_guest_ptr + len);
    if (c == '\0') break;
    if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
      REXLOG_INFO("[InteractGlyph] [RESOURCE-LOAD-TRACE] sub_8222CF18 entered, name=<non-printable> occurrence=#{}",
                  n + 1);
      return;
    }
    ++len;
  }
  if (len == 0 || len >= 256) {
    REXLOG_INFO("[InteractGlyph] [RESOURCE-LOAD-TRACE] sub_8222CF18 entered, name=<empty-or-too-long> occurrence=#{}",
                n + 1);
    return;
  }

  const char* name = reinterpret_cast<const char*>(base + name_guest_ptr);
  SetLastResourceName(name);
  REXLOG_INFO("[InteractGlyph] [RESOURCE-LOAD-TRACE] sub_8222CF18 entered, name=\"{}\" kbm_active={} occurrence=#{}",
              name, IsKbmActive(), n + 1);
}


// ===========================================================================
// 2026-11-15 Live glyph switching
// ===========================================================================
// Goal: KBM <-> controller glyphs swap instantly, mid-session, whenever the
// player touches the other device -- without the old dependency on the VFS
// bank redirect (which the engine's Permanent Bank Mounter only honours ONCE,
// at boot; see the long comment at the top of this file).
//
// Mechanism (all on one background worker thread, woken the instant
// SetActiveInputDevice() sees a real device change):
//
//  1. Bank textures (icon_button_a/b/x/y, the 24x24 inline tutorial glyphs,
//     DPad, ... -- every entry of gui_textures.bnk whose bytes differ between
//     the KBM bank and the original x360 bank, at the same offset/size):
//     both variants' pixel payloads are read once from the two host bank
//     files (extracted/data/art/_gui_kbm and _gui_x360). Entry boundaries come
//     from gui_bank_x360_layout.h (the original catalog, which never changes).
//     The engine keeps a texture's payload verbatim at a 4 KB-aligned guest
//     PHYSICAL address (proven by the 2026-09-12 [DIAG5] GPU-cache rounds:
//     icon_button_a's bytes found byte-identical at 0x133E1000-class
//     addresses), so the worker walks the committed regions of the guest
//     physical heap, page by page, checks a 16-byte discriminating probe for
//     each target and, on a probe hit, verifies the FULL payload byte-for-byte
//     before recording an instance. A switch rewrites each instance with the
//     other variant (only after re-verifying it still holds the exact bytes we
//     expect -- never writes over memory that is not one of the two known
//     payloads) through Memory::TranslatePhysical, then calls
//     Memory::TriggerPhysicalMemoryCallbacks for the 0xA0000000/0xC0000000
//     views -- the same invalidation path a guest CPU write takes -- so the
//     GPU texture cache re-uploads the texture on its next use. Bytes written
//     are always verbatim bank bytes, so the seven repaired inline BC3
//     payloads are reproduced exactly in KBM mode and the untouched originals
//     in controller mode. Rescans run right after a switch (textures streamed
//     in when a menu opens are caught within ~0.5 s) and then periodically.
//
//  2. abyx.tex (shared A/B/X/Y menu atlas): the KBM bank stores a different
//     256x256 RGBA8 entry (appended at the end of the bank, and the KBM
//     gui_texture_headers.bnk -- which is what is RAM-resident, since the
//     redirect is now pinned to KBM -- describes that format). The original
//     x360 abyx is a 0xB0 (undecoded codec) entry, so it can not be written
//     into the RGBA8 texture. Controller mode instead gets an RGBA8 atlas
//     COMPOSED AT RUNTIME from the original x360 icon_button_a/b/y/x.tex
//     payloads in the same v3 quadrant layout the KBM atlas uses (A/B/Y/X at
//     (0,0)/(128,0)/(0,128)/(128,128)), plus B also at (64,0) where the
//     original (uncorrected) Back-prompt UV samples. This is a faithful
//     reconstruction from the game's own controller art, not a byte copy of
//     the original atlas -- see AI_HANDOFF.md "Known limits".
//
//  3. Anark FAC render objects (selected-row A badge etc., kAnarkRenderBindings):
//     OnAnarkRenderObjectCreate() (called from sub_828152D0 for EVERY device
//     now) remembers every render object whose ORIGINAL model/hash/UV matches
//     a rule, applying the correction only while KBM is active (unchanged
//     behaviour at creation). On a device switch the worker re-validates each
//     tracked object (hash at +512 and the four corner UV pairs at
//     +48/+112/+176/+240 must exactly equal either the original or the
//     corrected state) and rewrites hash + corners + the dirty byte (+548)
//     the same way sub_82814BA8 would. sub_828156B8 (the draw) reads +512 and
//     the corners on every draw, so the next frame shows the other glyph.
//     Restoring the controller state writes back the object's own original
//     values, so controller mode is exactly the original game.
//
// The VFS bank redirect is pinned to _gui_kbm (kPinBankRedirectToKbm): with a
// once-only mount, following the device there could only ever create a
// mismatched headers/textures pair; all live switching now happens in memory.
namespace {

constexpr bool kEnableLiveBankTextureSwap = true;
constexpr bool kEnableLiveAbyxSwap = true;
constexpr bool kEnableLiveAnarkRebind = true;
constexpr uint32_t kLiveAbyxHash = 222321799u;  // 0x0D405C87, art\gui\persistant\abyx.tex
constexpr uint32_t kLiveAbyxPayloadSize = 256u * 256u * 4u;
constexpr uint32_t kAnarkUnitConstAddr = 0x820994C0u;  // lfs f12,-27456(r9) in sub_82814BA8
constexpr uint32_t kAnarkCornerOffsets[8] = {48, 52, 112, 116, 176, 180, 240, 244};
constexpr size_t kAnarkLiveMaxEntries = 2048;

int64_t LiveNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t LiveBe32(const std::vector<uint8_t>& v, size_t o) {
  return (uint32_t(v[o]) << 24) | (uint32_t(v[o + 1]) << 16) | (uint32_t(v[o + 2]) << 8) |
         uint32_t(v[o + 3]);
}

struct LiveTexTarget {
  std::string name;
  uint32_t size = 0;   // payload bytes
  uint32_t probe = 0;  // 16-byte discriminating probe offset within the payload
  std::vector<uint8_t> x360;
  std::vector<uint8_t> kbm;
};

struct LiveTexInstance {
  uint32_t phys = 0;
  uint32_t target = 0;
  bool is_kbm = false;
};

// Worker-thread-only state (no lock needed: only LiveGlyphWorker touches it).
std::vector<LiveTexTarget> g_live_targets;
std::vector<LiveTexInstance> g_live_instances;
bool g_live_targets_loaded = false;

// Wake-up signalling (any thread -> worker).
std::mutex g_live_wake_mutex;
std::condition_variable g_live_wake_cv;
bool g_live_wake = false;
std::atomic<bool> g_live_worker_started{false};

// Anark render-object registry (game thread writes, worker reads/writes).
struct AnarkLiveEntry {
  uint32_t object = 0;
  uint32_t rule = 0;
  double original[4] = {0, 0, 0, 0};  // su, sv, u, v exactly as the FAC supplied them
};
std::mutex g_anark_live_mutex;
std::vector<AnarkLiveEntry> g_anark_live;

std::vector<uint8_t> LiveReadWholeFile(const std::filesystem::path& path) {
  std::vector<uint8_t> out;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return out;
  std::ifstream f(path, std::ios::binary);
  if (!f) return out;
  f.seekg(0, std::ios::end);
  const std::streamoff sz = f.tellg();
  if (sz <= 0) return out;
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(out.data()), sz);
  if (!f) out.clear();
  return out;
}

// Payload range of a bank entry: kind-7 wrapper (7, 48, len) -> +48, or a
// plain 4-byte big-endian size prefix (== entry size - 4) -> +4. Anything else
// (e.g. the 0xB0 kind-1/3 codec) is not a raw GPU payload and is skipped.
bool LiveEntryPayload(const std::vector<uint8_t>& bank, uint32_t off, uint32_t size,
                      uint32_t& pay_off, uint32_t& pay_len) {
  if (size < 8 || size_t(off) + size > bank.size()) return false;
  if (size >= 48 && LiveBe32(bank, off) == 7u && LiveBe32(bank, off + 4) == 48u) {
    const uint32_t len = LiveBe32(bank, off + 8);
    if (len != 0 && len <= size - 48) {
      pay_off = off + 48;
      pay_len = len;
      return true;
    }
  }
  if (LiveBe32(bank, off) == size - 4) {
    pay_off = off + 4;
    pay_len = size - 4;
    return true;
  }
  return false;
}

bool LiveChooseProbe(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                     uint32_t& probe) {
  const size_t n = std::min(a.size(), b.size());
  auto uniform = [](const uint8_t* p) {
    for (int i = 1; i < 16; ++i)
      if (p[i] != p[0]) return false;
    return true;
  };
  for (size_t o = 0; o + 16 <= n; o += 16) {
    if (std::memcmp(a.data() + o, b.data() + o, 16) == 0) continue;
    if (uniform(a.data() + o) || uniform(b.data() + o)) continue;
    probe = static_cast<uint32_t>(o);
    return true;
  }
  return false;
}

const GuiBankEntry* LiveFindX360Entry(const char* name) {
  for (const auto& e : kGuiBankX360Entries) {
    const char* a = e.name;
    const char* b = name;
    while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                           std::tolower(static_cast<unsigned char>(*b))) {
      ++a;
      ++b;
    }
    if (*a == 0 && *b == 0) return &e;
  }
  return nullptr;
}

// Controller-mode abyx atlas, composed from the original x360 discrete
// icon_button_*.tex payloads (64x64, tiled, pitch 64) into a 256x256 tiled
// RGBA8 canvas -- the exact inverse of how the KBM v3 atlas was built
// (Claude outputs/apply_abyx_kbm_patch.py; same GetTiledOffset2D()).
std::vector<uint8_t> LiveComposeX360Abyx(const std::vector<uint8_t>& x360) {
  struct Cell {
    const char* name;
    uint32_t x, y;
  };
  static const Cell kCells[] = {
      {"Art\\GUI\\Controller\\icon_button_a.tex", 0, 0},
      {"Art\\GUI\\Controller\\icon_button_b.tex", 128, 0},
      {"Art\\GUI\\Controller\\icon_button_y.tex", 0, 128},
      {"Art\\GUI\\Controller\\icon_button_x.tex", 128, 128},
      // Original (uncorrected) Back-prompt UV: U=0.25 (anark_glyph_bindings.h
      // back_shift rule 0x3E800000 -> 0x3F000000) samples x=64..127.
      {"Art\\GUI\\Controller\\icon_button_b.tex", 64, 0},
  };
  std::vector<uint8_t> linear(kLiveAbyxPayloadSize, 0);
  for (const auto& cell : kCells) {
    const GuiBankEntry* e = LiveFindX360Entry(cell.name);
    uint32_t po = 0, pl = 0;
    if (!e || !LiveEntryPayload(x360, e->offset, e->size, po, pl) || pl != 64u * 64u * 4u) {
      return {};
    }
    const uint8_t* tiled = x360.data() + po;
    for (uint32_t y = 0; y < 64; ++y) {
      for (uint32_t x = 0; x < 64; ++x) {
        const int32_t src = GetTiledOffset2D(int32_t(x), int32_t(y), 64, 2);
        uint8_t* dst = &linear[((cell.y + y) * 256u + cell.x + x) * 4u];
        std::memcpy(dst, tiled + src, 4);
      }
    }
  }
  std::vector<uint8_t> out(kLiveAbyxPayloadSize, 0);
  TilePixels(linear.data(), out.data(), 256, 256, 4);
  return out;
}

void LiveLoadTargets() {
  g_live_targets_loaded = true;
  const std::filesystem::path root = GetGameRootDir();
  if (root.empty()) {
    REXLOG_WARN("[GlyphSystem] [LiveGlyph] game root not found; bank texture live swap disabled");
    return;
  }
  const auto art = root / "extracted" / "data" / "art";
  std::vector<uint8_t> x360 = LiveReadWholeFile(art / "_gui_x360" / "gui_textures.bnk");
  std::vector<uint8_t> kbm = LiveReadWholeFile(art / "_gui_kbm" / "gui_textures.bnk");
  if (x360.size() != kGuiBankX360FileSize || kbm.size() < x360.size()) {
    REXLOG_WARN("[GlyphSystem] [LiveGlyph] unexpected bank sizes (x360={} expected {}, kbm={}); "
                "bank texture live swap disabled",
                x360.size(), kGuiBankX360FileSize, kbm.size());
    return;
  }

  for (const auto& e : kGuiBankX360Entries) {
    if (size_t(e.offset) + e.size > x360.size()) continue;
    if (std::memcmp(x360.data() + e.offset, kbm.data() + e.offset, e.size) == 0) continue;
    uint32_t xo = 0, xl = 0, ko = 0, kl = 0;
    if (!LiveEntryPayload(x360, e.offset, e.size, xo, xl) ||
        !LiveEntryPayload(kbm, e.offset, e.size, ko, kl) || xo != ko || xl != kl) {
      continue;  // not an in-place raw payload swap (catalog area, abyx, ...)
    }
    LiveTexTarget t;
    t.name = e.name;
    t.size = xl;
    t.x360.assign(x360.begin() + xo, x360.begin() + xo + xl);
    t.kbm.assign(kbm.begin() + ko, kbm.begin() + ko + kl);
    if (!LiveChooseProbe(t.x360, t.kbm, t.probe)) continue;
    REXLOG_INFO("[GlyphSystem] [LiveGlyph] target '{}' payload={} bytes probe=+{}", t.name, t.size,
                t.probe);
    g_live_targets.push_back(std::move(t));
  }

  if (kEnableLiveAbyxSwap) {
    // KBM abyx = the appended entry(ies) after the original layout: walk the
    // size-prefixed chain and accept exactly one 256x256 RGBA8 payload.
    std::vector<uint32_t> candidates;
    size_t pos = x360.size();
    while (pos + 4 <= kbm.size()) {
      const uint32_t len = LiveBe32(kbm, pos);
      if (len == 0 || pos + 4 + size_t(len) > kbm.size()) break;
      if (len == kLiveAbyxPayloadSize) candidates.push_back(static_cast<uint32_t>(pos + 4));
      pos += 4 + size_t(len);
    }
    std::vector<uint8_t> composed = LiveComposeX360Abyx(x360);
    if (candidates.size() == 1 && composed.size() == kLiveAbyxPayloadSize) {
      LiveTexTarget t;
      t.name = "Art\\GUI\\Persistant\\abyx.tex (KBM RGBA8 <-> composed x360)";
      t.size = kLiveAbyxPayloadSize;
      t.kbm.assign(kbm.begin() + candidates[0], kbm.begin() + candidates[0] + kLiveAbyxPayloadSize);
      t.x360 = std::move(composed);
      if (LiveChooseProbe(t.x360, t.kbm, t.probe)) {
        REXLOG_INFO("[GlyphSystem] [LiveGlyph] target '{}' kbm_off=0x{:X} probe=+{}", t.name,
                    candidates[0], t.probe);
        g_live_targets.push_back(std::move(t));
      }
    } else {
      REXLOG_WARN("[GlyphSystem] [LiveGlyph] abyx live swap disabled (appended 256x256 candidates={}, "
                  "composed={} bytes)",
                  candidates.size(), composed.size());
    }
  }
  REXLOG_INFO("[GlyphSystem] [LiveGlyph] {} live-swappable bank texture(s) loaded", g_live_targets.size());
}

void LiveInvalidateGpu(rex::memory::Memory* mem, uint32_t phys, uint32_t length) {
  // Same notification a guest CPU write to a watched page produces. A page is
  // only watched in the view the game ALLOCATED it through (PhysicalHeap::
  // EnableAccessCallbacks skips pages inaccessible in that view), so notify
  // all three physical views. 2026-11-15 fix: the 4 KB-page view 0xE0000000
  // (where the GUI bank textures live) was missing, so the GPU never re-read a
  // swapped glyph that had already been uploaded. 0xE0000000 maps physical
  // address + 0x1000 (PhysicalHeap::GetPhysicalAddress).
  for (uint32_t view : {0xA0000000u, 0xC0000000u}) {
    mem->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        view + phys, length, true, false);
  }
  if (phys >= 0x1000u) {
    mem->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        0xE0000000u + (phys - 0x1000u), length, true, false);
  }
}

// Returns false if the instance no longer holds a known payload (drop it).
bool LiveApplyInstance(rex::memory::Memory* mem, LiveTexInstance& inst, bool want_kbm) {
  const LiveTexTarget& t = g_live_targets[inst.target];
  uint8_t* host = mem->TranslatePhysical(inst.phys);
  const std::vector<uint8_t>& current = inst.is_kbm ? t.kbm : t.x360;
  if (std::memcmp(host, current.data(), t.size) != 0) return false;
  if (inst.is_kbm == want_kbm) return true;
  const std::vector<uint8_t>& next = want_kbm ? t.kbm : t.x360;
  std::memcpy(host, next.data(), t.size);
  LiveInvalidateGpu(mem, inst.phys, t.size);
  inst.is_kbm = want_kbm;
  return true;
}

void LiveApplyKnownInstances(rex::memory::Memory* mem, bool want_kbm) {
  size_t changed = 0;
  auto it = g_live_instances.begin();
  while (it != g_live_instances.end()) {
    const bool was = it->is_kbm;
    if (!LiveApplyInstance(mem, *it, want_kbm)) {
      it = g_live_instances.erase(it);
      continue;
    }
    if (was != it->is_kbm) ++changed;
    ++it;
  }
  if (changed) {
    REXLOG_INFO("[GlyphSystem] [LiveGlyph] swapped {} resident glyph texture(s) -> {}", changed,
                want_kbm ? "KBM" : "controller");
  }
}

void LiveScanPhysical(rex::memory::Memory* mem, bool want_kbm) {
  if (g_live_targets.empty()) return;
  rex::memory::VirtualHeap* heap = mem->GetPhysicalHeap();
  if (!heap) return;
  const uint32_t page = heap->page_size() ? heap->page_size() : 4096u;
  const uint64_t heap_end = uint64_t(heap->heap_base()) + heap->heap_size();
  std::vector<LiveTexInstance> found;
  uint64_t addr = heap->heap_base();
  size_t regions = 0;
  while (addr < heap_end && regions < 1000000) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(static_cast<uint32_t>(addr), &info)) break;
    uint64_t region = info.region_size ? info.region_size : page;
    if (addr + region > heap_end) region = heap_end - addr;
    ++regions;
    if (info.state & rex::memory::kMemoryAllocationCommit) {
      const uint64_t end = addr + region;
      for (uint64_t p = addr; p < end; p += page) {
        const uint8_t* host = mem->TranslatePhysical(static_cast<uint32_t>(p));
        for (uint32_t ti = 0; ti < g_live_targets.size(); ++ti) {
          const LiveTexTarget& t = g_live_targets[ti];
          if (p + t.size > end) continue;
          const uint8_t* probe = host + t.probe;
          bool is_kbm;
          if (std::memcmp(probe, t.kbm.data() + t.probe, 16) == 0) {
            is_kbm = true;
          } else if (std::memcmp(probe, t.x360.data() + t.probe, 16) == 0) {
            is_kbm = false;
          } else {
            continue;
          }
          const std::vector<uint8_t>& ref = is_kbm ? t.kbm : t.x360;
          if (std::memcmp(host, ref.data(), t.size) != 0) continue;
          found.push_back({static_cast<uint32_t>(p), ti, is_kbm});
        }
      }
    }
    addr += region;
  }

  static std::atomic<int> s_scan_logs{0};
  if (found.size() != g_live_instances.size() && s_scan_logs.fetch_add(1) < 40) {
    std::string where;
    for (size_t i = 0; i < found.size() && i < 12; ++i) {
      if (!where.empty()) where += ", ";
      const std::string& nm = g_live_targets[found[i].target].name;
      const size_t slash = nm.rfind('\\');
      char addr_buf[32];
      std::snprintf(addr_buf, sizeof(addr_buf), "@0x%08X(", found[i].phys);
      where += (slash == std::string::npos ? nm : nm.substr(slash + 1)) + addr_buf +
               (found[i].is_kbm ? "kbm)" : "x360)");
    }
    REXLOG_INFO("[GlyphSystem] [LiveGlyph] physical scan: {} region(s), {} resident glyph texture(s): {}",
                regions, found.size(), where);
  }
  g_live_instances = std::move(found);
  LiveApplyKnownInstances(mem, want_kbm);
}

void AnarkComputeCorners(const uint8_t* base, const double in[4], uint32_t out[8]) {
  // Mirrors sub_82814BA8 (fable2_recomp.160.cpp) exactly, including its
  // double(float(...)) rounding steps and its 1.0 constant read from guest
  // memory, so the bits written are what the game itself would store.
  const uint32_t one_bits = GuestLoadU32(base, kAnarkUnitConstAddr);
  float one_f;
  std::memcpy(&one_f, &one_bits, 4);
  const double one = double(one_f);
  const double su = in[0], sv = in[1], u = in[2], v = in[3];
  const double f10 = double(float(sv + v));
  const double f11 = double(float(su + u));
  const double f13 = double(float(one - v));
  const double f12 = double(float(one - f10));
  const float c[8] = {float(u), float(f13), float(f11), float(f13),
                      float(u), float(f12), float(f11), float(f12)};
  for (int i = 0; i < 8; ++i) std::memcpy(&out[i], &c[i], 4);
}

void LiveApplyAnark(uint8_t* base, bool want_kbm) {
  if (!kEnableLiveAnarkRebind || base == nullptr) return;
  std::lock_guard<std::mutex> lock(g_anark_live_mutex);
  size_t changed = 0;
  auto it = g_anark_live.begin();
  while (it != g_anark_live.end()) {
    const AnarkRenderBinding& rule = kAnarkRenderBindings[it->rule];
    const uint32_t obj = it->object;
    if (!IsValidGuestWritePtr(base, obj, 552)) {
      it = g_anark_live.erase(it);
      continue;
    }
    const double corrected[4] = {rule.after[0], rule.after[1], rule.after[2], rule.after[3]};
    uint32_t orig_c[8], corr_c[8], cur_c[8];
    AnarkComputeCorners(base, it->original, orig_c);
    AnarkComputeCorners(base, corrected, corr_c);
    for (int i = 0; i < 8; ++i) cur_c[i] = GuestLoadU32(base, obj + kAnarkCornerOffsets[i]);
    const uint32_t cur_hash = GuestLoadU32(base, obj + 512);
    const bool is_orig = cur_hash == rule.hash && std::memcmp(cur_c, orig_c, sizeof(cur_c)) == 0;
    const bool is_corr = cur_hash == kLiveAbyxHash && std::memcmp(cur_c, corr_c, sizeof(cur_c)) == 0;
    if (!is_orig && !is_corr) {
      it = g_anark_live.erase(it);  // freed / reused / rebuilt by the game
      continue;
    }
    if (want_kbm != is_corr) {
      const uint32_t* target = want_kbm ? corr_c : orig_c;
      for (int i = 0; i < 8; ++i) GuestStoreU32(base, obj + kAnarkCornerOffsets[i], target[i]);
      GuestStoreU32(base, obj + 512, want_kbm ? kLiveAbyxHash : rule.hash);
      GuestStoreU8(base, obj + 548, 1);
      ++changed;
    }
    ++it;
  }
  if (changed) {
    REXLOG_INFO("[GlyphSystem] [LiveGlyph] re-bound {} Anark render object(s) -> {} ({} tracked)", changed,
                want_kbm ? "KBM" : "controller", g_anark_live.size());
  }
}

void LiveGlyphWorker() {
  bool applied_kbm = IsKbmActive();
  bool first = true;
  int64_t last_scan_ms = 0;
  int64_t quick_until_ms = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_live_wake_mutex);
      g_live_wake_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return g_live_wake; });
      g_live_wake = false;
    }
    auto* kstate = rex::system::kernel_state();
    if (!kstate) continue;
    rex::memory::Memory* mem = kstate->memory();
    if (!mem) continue;

    const int64_t now = LiveNowMs();
    const bool want_kbm = IsKbmActive();
    const bool switched = first || want_kbm != applied_kbm;
    first = false;

    if (switched) {
      LiveApplyAnark(mem->virtual_membase(), want_kbm);
      quick_until_ms = now + 6000;
    }
    if (!kEnableLiveBankTextureSwap) {
      applied_kbm = want_kbm;
      continue;
    }
    if (!g_live_targets_loaded) LiveLoadTargets();
    if (switched) LiveApplyKnownInstances(mem, want_kbm);
    applied_kbm = want_kbm;

    // Rescan cadence: fast right after a switch (menus stream textures in
    // when opened), slower steady state. In KBM mode the pinned KBM bank
    // already loads KBM bytes, so only a slow safety scan is needed.
    const int64_t interval = now < quick_until_ms ? 500 : (want_kbm ? 15000 : 3000);
    if (switched || now - last_scan_ms >= interval) {
      LiveScanPhysical(mem, want_kbm);
      last_scan_ms = LiveNowMs();
    }
  }
}

void EnsureLiveGlyphWorker() {
  bool expected = false;
  if (!g_live_worker_started.compare_exchange_strong(expected, true)) return;
  std::thread(LiveGlyphWorker).detach();
  REXLOG_INFO("[GlyphSystem] [LiveGlyph] live glyph worker started");
}

}  // namespace

void NotifyLiveGlyphDeviceChanged() {
  EnsureLiveGlyphWorker();
  {
    std::lock_guard<std::mutex> lk(g_live_wake_mutex);
    g_live_wake = true;
  }
  g_live_wake_cv.notify_one();
}

bool OnAnarkRenderObjectCreate(uint32_t model, uint32_t object, uint32_t& hash, double& su,
                               double& sv, double& u, double& v) {
  const double input[4] = {su, sv, u, v};
  for (uint32_t r = 0; r < std::size(kAnarkRenderBindings); ++r) {
    const AnarkRenderBinding& rule = kAnarkRenderBindings[r];
    if (rule.model != model || rule.hash != hash) continue;
    bool matches = true;
    for (unsigned i = 0; i < 4; ++i)
      matches &= std::isfinite(input[i]) && std::abs(input[i] - rule.before[i]) < 0.000001;
    if (!matches) continue;
    if (kEnableLiveAnarkRebind) {
      std::lock_guard<std::mutex> lock(g_anark_live_mutex);
      auto existing = std::find_if(g_anark_live.begin(), g_anark_live.end(),
                                   [object](const AnarkLiveEntry& e) { return e.object == object; });
      if (existing != g_anark_live.end()) g_anark_live.erase(existing);
      if (g_anark_live.size() >= kAnarkLiveMaxEntries) g_anark_live.erase(g_anark_live.begin());
      AnarkLiveEntry entry;
      entry.object = object;
      entry.rule = r;
      for (int i = 0; i < 4; ++i) entry.original[i] = input[i];
      g_anark_live.push_back(entry);
    }
    break;
  }
  EnsureLiveGlyphWorker();
  if (!IsKbmActive()) return false;
  return CorrectAnarkRenderBinding(model, hash, su, sv, u, v);
}

} // namespace fable2::glyph
