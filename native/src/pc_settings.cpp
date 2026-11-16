// fable2 - native "PC Settings" branch in the front-end main menu (2026-11-15).
// See AI_HANDOFF.md "PC Settings main menu" for the full history.
//
// Menu layout (two levels, like the stock "Subtitles" branch):
//   Root > "PC Settings" > "Mouse Sensitivity: 1.0x", "V-Sync: On", ...
// fable2_pc_settings_style = "list" (default): pressing a setting swaps the
// children of "PC Settings" for that setting's values (current one emphasised
// and highlighted); pressing a value applies + saves it and swaps the settings
// list back. "cycle": pressing a setting steps to its next value in place.
// Swapping uses the stock repopulation path: g_Menu:RemoveChildrenOfItem()
// (String1 = parent) and then the normal add-item call. (Three levels do not work: the expandable menu
// is an accordion -- expandablemenuformatting AutoCollapse() collapses any
// other open entry, i.e. the "PC Settings" parent, instead of opening a child.)
//
// Game functions used (all verified against the recompiled code):
//   sub_826CB808(menu, &id, &text, &icon, &parentId, 0, 2, element)
//       add item -> Lua "g_Menu:SpawnButton()" (String1..4 = id/text/icon/parent)
//   sub_82301D40(&textOut, &keyString)   text from key (consumes keyString).
//       Unknown keys display verbatim UNLESS they look numeric (digits, one '.',
//       leading '-'): those are looked up as numeric ids and crash.
//   sub_8259D6C8(lua, &attrName, &value, element)   set a string g_Param attribute
//   sub_823F8908(&luaCode, element)                 run Lua with that element
//       -> "g_Menu:RefreshButtonText()" (String1 = id, String2 = new text)
//   element = *(menu+20) -- only valid during the build / event handling.
//   lua     = *(*(*(*(0x83496928)+8)+80)+40) (same chain sub_826CB808 uses)
//
// Hooks (the generated functions are weak; originals via __imp__):
//   sub_826CBA78  menu set-selected: when called from the builder's last call
//                 (return address 0x826CAC98) the branch is appended there.
//   sub_826C81A0  menu event handler: our ids are handled here.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(fable2_pc_settings_menu, true, "Fable II",
    "Show the PC Settings branch in the front-end main menu.");
REXCVAR_DEFINE_STRING(fable2_pc_settings_style, "cycle", "Fable II",
    "PC Settings menu: 'list' = pressing a setting shows its values to pick from, "
    "'cycle' = pressing a setting steps to its next value.");

extern "C" {
REX_FUNC(__imp__sub_826CBA78);  // menu set-item-selected / emphasis (hooked)
REX_FUNC(__imp__sub_826C81A0);  // front-end main menu event handler (hooked)
REX_FUNC(sub_8222CF18);         // string ctor (dst, const char*, len=-1)
REX_FUNC(sub_82214F08);         // string dtor
REX_FUNC(sub_82301D40);         // text-from-key (dst, keyString*) -- consumes keyString
REX_FUNC(sub_821C6868);         // text dtor
REX_FUNC(sub_826CB808);         // menu add item
REX_FUNC(sub_8259D6C8);         // set string attribute on the Lua param element
REX_FUNC(sub_823F8908);         // run Lua code string on an element
}

namespace fable2::pc_settings {
namespace {

enum class Kind { kDouble, kBool, kInt, kString };

struct Option {
  const char* value;    // cvar value
  const char* display;  // shown after "Name: "
};

struct Setting {
  const char* id;
  const char* name;
  const char* cvar;
  Kind kind;
  std::vector<Option> options;
};

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> kSettings = {
      {"PCS_MouseSens", "Mouse Sensitivity", "fable2_mouse_sensitivity", Kind::kDouble,
       {{"0.25", "0.25x"}, {"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1.0x"},
        {"1.25", "1.25x"}, {"1.5", "1.5x"}, {"2.0", "2.0x"}, {"3.0", "3.0x"}}},
      {"PCS_MouseY", "Vertical Sensitivity", "fable2_mouse_y_scale", Kind::kDouble,
       {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1.0", "1.0x"}, {"1.5", "1.5x"}, {"2.0", "2.0x"}}},
      {"PCS_InvertY", "Invert Vertical Look", "fable2_mouse_invert_y", Kind::kBool,
       {{"false", "Off"}, {"true", "On"}}},
      {"PCS_Aniso", "Texture Filtering", "anisotropic_override", Kind::kInt,
       {{"0", "Off"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"}, {"5", "16x"}}},
      {"PCS_VSync", "V-Sync", "vsync", Kind::kBool, {{"true", "On"}, {"false", "Off"}}},
      // These two cvars are kRequiresRestart.
      // [FPS120] fable2_frame_rate (pc_features.cpp) replaces fable2_60fps here.
      {"PCS_FrameRate", "Frame Rate", "fable2_frame_rate", Kind::kInt,
       {{"30", "30 FPS (restart)"}, {"60", "60 FPS (restart)"}, {"120", "120 FPS (restart)"}}},
      {"PCS_AA", "Anti-Aliasing", "swap_post_effect", Kind::kString,
       {{"none", "Off (restart)"}, {"fxaa", "FXAA (restart)"}}},
  };
  return kSettings;
}

constexpr const char* kRootId = "PCS_Root";
constexpr const char* kRootText = "PC Settings";
constexpr const char* kMenuRootId = "Root";
constexpr const char* kIcon = "Art\\GUI\\icon_options.tga";
constexpr uint32_t kLuaRootGlobal = 0x83496928;

// --- guest memory helpers ---------------------------------------------------
inline uint32_t Load32(uint8_t* base, uint32_t a) {
  return std::byteswap(*reinterpret_cast<uint32_t*>(base + a));
}
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  *reinterpret_cast<uint32_t*>(base + a) = std::byteswap(v);
}
inline bool PlausiblePtr(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

// Guest stack frame for our calls: [0..191] call frame + string-object slots
// (same offsets the stock builder uses), [192..] scratch for our C strings.
// The game's string ctor copies them, so they only need to live per call.
// (No heap allocation: SystemHeapAlloc from the game thread faulted.)
constexpr uint32_t kStrBufSize = 128;
constexpr uint32_t kStrBuf = 192;
constexpr uint32_t kCodeBuf = kStrBuf + 4 * kStrBufSize;
constexpr uint32_t kCodeBufSize = 1024;
constexpr uint32_t kFrame = kCodeBuf + kCodeBufSize;
constexpr uint32_t kSlotParent = 96, kSlotIcon = 100, kSlotId = 104, kSlotText = 108,
                   kSlotKey = 112;

struct ScopedCtx {
  PPCContext& ctx;
  PPCContext saved;
  ScopedCtx(PPCContext& c, uint8_t* base) : ctx(c), saved(c) {
    const uint32_t sp = ctx.r1.u32 - kFrame;
    Store32(base, sp, ctx.r1.u32);  // back chain
    ctx.r1.u64 = sp;
    ctx.lr = 0x826CACA8;  // any in-builder address; callees only save/restore it
  }
  ~ScopedCtx() { ctx = saved; }
};

uint32_t StackStr(PPCContext& ctx, uint8_t* base, int n, const char* s) {
  const uint32_t addr = ctx.r1.u32 + kStrBuf + uint32_t(n) * kStrBufSize;
  const size_t len = std::min<size_t>(std::strlen(s), kStrBufSize - 1);
  std::memcpy(base + addr, s, len);
  base[addr + len] = 0;
  return addr;
}

void StrCtor(PPCContext& ctx, uint8_t* base, uint32_t slot, uint32_t guest_str) {
  ctx.r3.u64 = ctx.r1.u32 + slot;
  ctx.r4.u64 = guest_str;
  ctx.r5.s64 = -1;
  sub_8222CF18(ctx, base);
}
void StrDtor(PPCContext& ctx, uint8_t* base, uint32_t slot) {
  ctx.r3.u64 = ctx.r1.u32 + slot;
  sub_82214F08(ctx, base);
}

// sub_82B3A7A0's rule; such keys are taken as numeric text ids and crash.
bool LooksNumeric(const char* s) {
  bool dot = false;
  for (int i = 0; s[i]; ++i) {
    const char c = s[i];
    if (c == '.' && !dot) { dot = true; continue; }
    if (c == '-' && i == 0) continue;
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool AddItem(PPCContext& ctx, uint8_t* base, uint32_t menu, const char* id, const char* text,
             const char* parent) {
  if (LooksNumeric(text)) {
    REXLOG_WARN("[PCSettings] skipped '{}': label '{}' looks numeric", id, text);
    return false;
  }
  const uint32_t g_id = StackStr(ctx, base, 0, id), g_text = StackStr(ctx, base, 1, text),
                 g_icon = StackStr(ctx, base, 2, kIcon), g_parent = StackStr(ctx, base, 3, parent);
  StrCtor(ctx, base, kSlotParent, g_parent);
  StrCtor(ctx, base, kSlotIcon, g_icon);
  StrCtor(ctx, base, kSlotId, g_id);
  StrCtor(ctx, base, kSlotKey, g_text);
  ctx.r3.u64 = ctx.r1.u32 + kSlotText;
  ctx.r4.u64 = ctx.r1.u32 + kSlotKey;
  sub_82301D40(ctx, base);            // destroys the key string itself
  Store32(base, ctx.r1.u32 + 84, 0);  // as the stock builder does
  ctx.r3.u64 = menu;
  ctx.r4.u64 = ctx.r1.u32 + kSlotId;
  ctx.r5.u64 = ctx.r1.u32 + kSlotText;
  ctx.r6.u64 = ctx.r1.u32 + kSlotIcon;
  ctx.r7.u64 = ctx.r1.u32 + kSlotParent;
  ctx.r8.s64 = 0;
  ctx.r9.s64 = 2;
  ctx.r10.u64 = Load32(base, menu + 20);
  sub_826CB808(ctx, base);
  ctx.r3.u64 = ctx.r1.u32 + kSlotText;
  sub_821C6868(ctx, base);
  StrDtor(ctx, base, kSlotId);
  StrDtor(ctx, base, kSlotIcon);
  StrDtor(ctx, base, kSlotParent);
  return true;
}

uint32_t LuaState(uint8_t* base) {
  uint32_t p = Load32(base, kLuaRootGlobal);
  if (!PlausiblePtr(p)) return 0;
  p = Load32(base, p + 8);
  if (!PlausiblePtr(p)) return 0;
  p = Load32(base, p + 80);
  if (!PlausiblePtr(p)) return 0;
  p = Load32(base, p + 40);
  return PlausiblePtr(p) ? p : 0;
}

// sub_8259D6C8(lua, &name, &value, element) with plain C strings.
void SetParamString(PPCContext& ctx, uint8_t* base, uint32_t lua, uint32_t element,
                    const char* name, const char* value) {
  StrCtor(ctx, base, kSlotKey, StackStr(ctx, base, 0, name));
  StrCtor(ctx, base, kSlotId, StackStr(ctx, base, 1, value));
  ctx.r3.u64 = lua;
  ctx.r4.u64 = ctx.r1.u32 + kSlotKey;
  ctx.r5.u64 = ctx.r1.u32 + kSlotId;
  ctx.r6.u64 = element;
  sub_8259D6C8(ctx, base);
  StrDtor(ctx, base, kSlotId);
  StrDtor(ctx, base, kSlotKey);
}

// Runs a Lua chunk the way the game runs "g_Menu:SpawnButton()" etc.
// (sub_823F8908(&code, element)); String1/String2 must be set beforehand when
// the chunk reads them.
bool RunLua(PPCContext& ctx, uint8_t* base, uint32_t element, const std::string& code) {
  if (code.size() >= kCodeBufSize) {
    REXLOG_WARN("[PCSettings] Lua chunk too long ({} bytes)", code.size());
    return false;
  }
  const uint32_t addr = ctx.r1.u32 + kCodeBuf;
  std::memcpy(base + addr, code.data(), code.size());
  base[addr + code.size()] = 0;
  StrCtor(ctx, base, kSlotKey, addr);
  ctx.r3.u64 = ctx.r1.u32 + kSlotKey;
  ctx.r4.u64 = element;
  sub_823F8908(ctx, base);
  StrDtor(ctx, base, kSlotKey);
  return true;
}

struct MenuCtx {
  uint32_t menu = 0, element = 0, lua = 0;
  bool ok() const { return PlausiblePtr(element) && lua; }
};
MenuCtx GetMenuCtx(uint8_t* base, uint32_t menu) {
  MenuCtx m;
  m.menu = menu;
  m.element = Load32(base, menu + 20);
  m.lua = LuaState(base);
  return m;
}

// Relabel an existing item in place. Same updates the stock
// g_Menu:RefreshButtonText() makes (item.TextTag + 'TextID' on the title of
// every row component showing the item), plus the formatter's own immediate
// A TextID change starts a text cross-fade inside the row that only advances
// while the row is being re-rendered (user video: the new value appeared --
// fading over the old one -- only while scrolling animated the rows). So each
// such row gets a tiny (0.02 px, alternating sign) position.x tween over 0.3 s
// via the menu's own g_MenuFormatter:AnimatePropertyTo (expandablemenuanimation
// proto5: element, property, end_val, duration), which re-renders it every
// frame until the fade completes. No DirtyLayout/DirtyHighlight: forcing a full
// relayout left stale rows drawn on top of each other.
bool RenameItem(PPCContext& ctx, uint8_t* base, const MenuCtx& m, const char* id,
                const char* text) {
  if (!m.ok()) return false;
  for (const char* p : {id, text})
    if (std::strchr(p, '\'') || std::strchr(p, '\\')) return false;  // keep the chunk well-formed
  // The row on screen is g_MenuFormatter.MenuItems[item.ComponentIdx] (that is
  // how onExpandMenuItem / onScrollDown find it). Re-run the formatter's own
  // per-row move into its current slot, AnimatePropertiesToIndex(comp,
  // comp.VisibleIndex) -- the exact call that made the new text appear while
  // scrolling (it also animates Icon.Title size/fontsize/truncate) -- plus a tiny
  // alternating Icon.Title size change so the text box re-lays out regardless.
  // Other comps still referencing the item only get the new TextID.
  return RunLua(ctx, base, m.element,
                std::string("local it = g_Menu:GetMenuItem('") + id + "') if it then "
                "local txt = '" + text + "' it.TextTag = txt "
                "local F = g_MenuFormatter "
                "local function settext(c) local t = getElement('Icon.Title', c.element) "
                "if t then setAttribute(t, 'TextID', txt) end return t end "
                "local main = it.ComponentIdx and F.MenuItems[it.ComponentIdx] "
                "if main and main.element then "
                "local t = settext(main) "
                "if t then pcall(function() local sz = getAttribute(t, 'size') "
                "if type(sz) == 'number' then local d = main.PCSNudge and -0.01 or 0.01 "
                "main.PCSNudge = not main.PCSNudge setAttribute(t, 'size', sz + d) end end) end "
                "if main.VisibleIndex then "
                "pcall(F.AnimatePropertiesToIndex, F, main, main.VisibleIndex, 0) end "
                "end "
                "for _, c in pairs(F.MenuItems) do "
                "if c ~= main and c.MenuItemRef == it and c.element then settext(c) end end "
                "end");
}

void RemoveChildren(PPCContext& ctx, uint8_t* base, const MenuCtx& m, const char* parent) {
  SetParamString(ctx, base, m.lua, m.element, "String1", parent);
  RunLua(ctx, base, m.element, "g_Menu:RemoveChildrenOfItem()");
}

// Moves the highlight to an item (index from the item tree, as the menu does).
void HighlightItem(PPCContext& ctx, uint8_t* base, const MenuCtx& m, const char* id) {
  RunLua(ctx, base, m.element,
         std::string("local m = g_Menu local it = m:GetMenuItem('") + id +
             "') if it then local v = m:GetMenuItemVisibleIdx(it) if v and v > 0 then "
             "m.CurrentHighlightIdx = v end end m:DirtyLayout() m:DirtyHighlight()");
}

void MarkEmphasis(PPCContext& ctx, uint8_t* base, const MenuCtx& m, const char* id, bool on) {
  StrCtor(ctx, base, kSlotId, StackStr(ctx, base, 0, id));
  ctx.r3.u64 = m.menu;
  ctx.r4.u64 = ctx.r1.u32 + kSlotId;
  ctx.r5.s64 = on ? 1 : 0;
  ctx.r6.u64 = m.element;
  __imp__sub_826CBA78(ctx, base);
  StrDtor(ctx, base, kSlotId);
}

// --- settings values --------------------------------------------------------
bool OptionMatches(const Setting& s, const Option& o, const std::string& cur) {
  try {
    switch (s.kind) {
      case Kind::kDouble:
        return std::abs(std::stod(cur) - std::stod(o.value)) < 1e-4;
      case Kind::kInt:
        return std::stoi(cur) == std::stoi(o.value);
      case Kind::kBool:
        return (cur == "true" || cur == "1") == (std::string(o.value) == "true");
      case Kind::kString:
        return cur == o.value;
    }
  } catch (...) {
  }
  return false;
}

int CurrentIndex(const Setting& s) {
  const std::string cur = rex::cvar::GetFlagByName(s.cvar);
  for (size_t i = 0; i < s.options.size(); ++i)
    if (OptionMatches(s, s.options[i], cur)) return int(i);
  return -1;
}

std::string Label(const Setting& s, int idx) {
  std::string v;
  if (idx >= 0) {
    v = s.options[size_t(idx)].display;
  } else {  // value not in the list (edited by hand): show it as-is
    v = rex::cvar::GetFlagByName(s.cvar);
    if (s.kind == Kind::kDouble) {
      try {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2fx", std::stod(v));
        v = buf;
      } catch (...) {
      }
    }
  }
  return std::string(s.name) + ": " + v;
}

std::filesystem::path ConfigPath() {
  wchar_t exe[32768] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
  if (!n || n >= 32768) return {};
  auto dir = std::filesystem::path(exe).parent_path();
  for (int level = 0; level < 7; ++level) {  // same search as Fable2App::OnConfigurePaths
    if (std::filesystem::is_regular_file(dir / "extracted" / "default.xex"))
      return dir / "native" / "fable2.toml";
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

// What "PC Settings" currently contains: -1 = the settings list, otherwise
// the index of the setting whose values are shown.
int g_view = -1;

std::string OptionId(size_t si, size_t oi) {
  return "PCS_Opt_" + std::to_string(si) + "_" + std::to_string(oi);
}

void AddSettingsList(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  for (const auto& s : Settings()) {
    const std::string label = Label(s, CurrentIndex(s));
    AddItem(ctx, base, menu, s.id, label.c_str(), kRootId);
  }
}

void AddAllItems(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  if (!PlausiblePtr(Load32(base, menu + 20))) {
    REXLOG_WARN("[PCSettings] menu element not set; branch skipped");
    return;
  }
  g_view = -1;
  if (!AddItem(ctx, base, menu, kRootId, kRootText, kMenuRootId)) return;
  AddSettingsList(ctx, base, menu);
  REXLOG_INFO("[PCSettings] added PC Settings with {} setting(s)", Settings().size());
}

void ShowSettingsList(PPCContext& ctx, uint8_t* base, const MenuCtx& m, const char* highlight) {
  RemoveChildren(ctx, base, m, kRootId);
  AddSettingsList(ctx, base, m.menu);
  g_view = -1;
  if (highlight) HighlightItem(ctx, base, m, highlight);
}

void ShowOptions(PPCContext& ctx, uint8_t* base, const MenuCtx& m, size_t si) {
  const Setting& s = Settings()[si];
  RemoveChildren(ctx, base, m, kRootId);
  for (size_t oi = 0; oi < s.options.size(); ++oi) {
    const std::string label = std::string(s.name) + ": " + s.options[oi].display;
    AddItem(ctx, base, m.menu, OptionId(si, oi).c_str(), label.c_str(), kRootId);
  }
  g_view = int(si);
  const int cur = CurrentIndex(s);
  const std::string focus = OptionId(si, cur >= 0 ? size_t(cur) : 0);
  if (cur >= 0) MarkEmphasis(ctx, base, m, focus.c_str(), true);
  HighlightItem(ctx, base, m, focus.c_str());
}

bool ApplyOption(const Setting& s, const Option& o) {
  const bool ok = rex::cvar::SetFlagByName(s.cvar, o.value);
  const auto path = ConfigPath();
  if (ok && !path.empty()) rex::cvar::SaveConfig(path);
  REXLOG_INFO("[PCSettings] {} = {} ({})", s.cvar, o.value, ok ? "applied" : "FAILED");
  return ok;
}

// Returns true if the event was a press on one of our items (handled).
bool HandleEvent(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t event) {
  if (!PlausiblePtr(event)) return false;
  const uint32_t type = Load32(base, event + 4);
  if (type == 302 || type == 141 || type == 140) return false;  // no item id in these
  const uint32_t str_obj = Load32(base, event + 0);
  if (!PlausiblePtr(str_obj)) return false;
  const uint32_t cstr = Load32(base, str_obj + 0);
  if (!PlausiblePtr(cstr)) return false;
  const char* id = reinterpret_cast<const char*>(base + cstr);
  if (std::strncmp(id, "PCS_", 4) != 0) return false;

  const MenuCtx m = GetMenuCtx(base, menu);
  if (!m.ok()) {
    REXLOG_WARN("[PCSettings] press on '{}' ignored (element=0x{:08X} lua=0x{:08X})", id,
                m.element, m.lua);
    return true;
  }
  const bool list_style = rex::cvar::GetFlagByName("fable2_pc_settings_style") == "list";

  // The branch itself: the menu opens/closes it; make sure it holds the list.
  if (std::strcmp(id, kRootId) == 0) {
    if (g_view >= 0) ShowSettingsList(ctx, base, m, nullptr);
    return true;
  }

  // A value from the options view.
  if (std::strncmp(id, "PCS_Opt_", 8) == 0) {
    size_t si = 0, oi = 0;
    if (std::sscanf(id + 8, "%zu_%zu", &si, &oi) == 2 && si < Settings().size() &&
        oi < Settings()[si].options.size()) {
      ApplyOption(Settings()[si], Settings()[si].options[oi]);
      ShowSettingsList(ctx, base, m, Settings()[si].id);
    }
    return true;
  }

  // A setting.
  for (size_t si = 0; si < Settings().size(); ++si) {
    const Setting& s = Settings()[si];
    if (std::strcmp(id, s.id) != 0) continue;
    if (list_style) {
      ShowOptions(ctx, base, m, si);
    } else {
      const int next = (CurrentIndex(s) + 1) % int(s.options.size());
      const bool ok = ApplyOption(s, s.options[size_t(next)]);
      const std::string label = Label(s, ok ? next : CurrentIndex(s));
      RenameItem(ctx, base, m, s.id, label.c_str());
    }
    return true;
  }
  return true;  // unknown PCS_ id: swallow
}

}  // namespace
}  // namespace fable2::pc_settings

// --- hooks (strong overrides of the weak generated functions) --------------
// The branch must be added WHILE the stock builder sub_826CA6E0 runs (element
// = *(menu+20) is only valid then). Its last call is sub_826CBA78 (mark the
// current Subtitles choice) returning to 0x826CAC98; append right after it.
constexpr uint64_t kBuilderLastMarkReturn = 0x826CAC98;

extern "C" REX_FUNC(sub_826CBA78) {
  const uint64_t lr = ctx.lr;
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_826CBA78(ctx, base);
  if (lr == kBuilderLastMarkReturn && REXCVAR_GET(fable2_pc_settings_menu)) {
    fable2::pc_settings::ScopedCtx scope(ctx, base);
    fable2::pc_settings::AddAllItems(ctx, base, menu);
  }
}

extern "C" REX_FUNC(sub_826C81A0) {
  const uint32_t menu = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  if (REXCVAR_GET(fable2_pc_settings_menu)) {
    fable2::pc_settings::ScopedCtx scope(ctx, base);
    if (fable2::pc_settings::HandleEvent(ctx, base, menu, event)) return;
  }
  __imp__sub_826C81A0(ctx, base);
}
