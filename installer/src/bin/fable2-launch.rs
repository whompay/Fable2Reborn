//! `Fable II.exe`: the launcher. A plain native Windows options dialog with the game's PC settings
//! (the cvars in `native/fable2.toml`, which the in-game PC Settings menu writes too). Play saves
//! them and starts the game the way `installer/launch.txt` says, without a console window.
//!
//! `Fable II.exe --play` skips the window and starts the game straight away.
//!
//! Updates: when the window opens, an installed copy asks its own installer in the background
//! whether GitHub has a newer version (`fable2-installer update --check`, the same check the setup
//! program does). If it has, an **Update** button appears; it runs `Update Fable II.cmd` (download
//! what changed, rebuild what that needs) in a console window and closes the launcher. Offline,
//! a development tree, or `FABLE2_NO_UPDATE_CHECK=1`: no check, no button.
//!
//! Dev builds: `fable2-launch.exe --settings-only --cwd <dir> --exe <game exe>` shows the same
//! window, saves the `native/fable2.toml` above `<dir>` and exits 0 on Play (the calling script then
//! starts the game itself), or exits 1 when the window is closed.
#![cfg_attr(windows, windows_subsystem = "windows")]
#![cfg_attr(not(windows), allow(dead_code))]

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

/// Where the port is published; the line under the buttons links there. Empty: no link, just
/// the note.
#[cfg_attr(not(windows), allow(dead_code))]
const REPO_URL: &str = "https://github.com/whompay/Fable2Reborn";
#[cfg_attr(not(windows), allow(dead_code))]
const REPO_TEXT: &str = "github.com/whompay/Fable2Reborn";

// ---- launch.txt ------------------------------------------------------------------------------

fn message(text: &str) {
    #[cfg(windows)]
    {
        #[link(name = "user32")]
        extern "system" {
            fn MessageBoxW(
                hwnd: *mut core::ffi::c_void,
                text: *const u16,
                caption: *const u16,
                kind: u32,
            ) -> i32;
        }
        let w = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<u16>>();
        let (t, c) = (w(text), w("Fable II Reborn"));
        // SAFETY: both strings are NUL-terminated UTF-16; MB_OK | MB_ICONERROR.
        unsafe { MessageBoxW(core::ptr::null_mut(), t.as_ptr(), c.as_ptr(), 0x10) };
    }
    #[cfg(not(windows))]
    eprintln!("{text}");
}

fn resolve(root: &Path, p: &str) -> PathBuf {
    let pb = PathBuf::from(p);
    if pb.is_absolute() {
        pb
    } else {
        p.split('/')
            .filter(|s| !s.is_empty())
            .fold(root.to_path_buf(), |a, s| a.join(s))
    }
}

struct Launch {
    root: PathBuf,
    exe: PathBuf,
    cwd: PathBuf,
    args: Vec<PathBuf>,
    env: Vec<(String, String)>,
    out: Option<String>,
    err: Option<String>,
    /// `--settings-only`: Play just saves and returns to the calling script.
    settings_only: bool,
}

impl Launch {
    fn load() -> Result<Self, String> {
        let me = std::env::current_exe().map_err(|e| e.to_string())?;
        let root = me.parent().ok_or("no folder")?.to_path_buf();
        let cfg = root.join("installer").join("launch.txt");
        let txt = std::fs::read_to_string(&cfg).map_err(|_| {
            "The game has not been built yet.\n\nRun Fable II Setup again to finish the installation."
                .to_string()
        })?;
        let (mut exe, mut cwd, mut out, mut err) = (None, None, None, None);
        let (mut exe_compat, mut cwd_compat) = (None, None);
        let mut args = Vec::new();
        let mut env = Vec::new();
        for line in txt.lines() {
            let line = line.trim_end();
            if line.starts_with('#') || line.is_empty() {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            match k {
                "exe" => exe = Some(resolve(&root, v)),
                "cwd" => cwd = Some(resolve(&root, v)),
                "exe_compat" => exe_compat = Some(resolve(&root, v)),
                "cwd_compat" => cwd_compat = Some(resolve(&root, v)),
                "arg" => args.push(resolve(&root, v)),
                "argv" => args.push(PathBuf::from(v)),
                "stdout" => out = Some(v.to_string()),
                "stderr" => err = Some(v.to_string()),
                "env" => {
                    if let Some((a, b)) = v.split_once('=') {
                        env.push((a.to_string(), b.to_string()));
                    }
                }
                _ => {}
            }
        }
        // The fast build needs AVX2/FMA; the compatible one runs everywhere else.
        let fast = fable2_installer::util::cpu_has_avx2();
        let (exe, cwd) = match (exe, exe_compat) {
            (Some(e), _) if fast => (e, cwd),
            (_, Some(e)) => (e, cwd_compat),
            (Some(_), None) => {
                return Err("This PC's processor has no AVX2, and only the AVX2 version of the game was built.\n\n\
                     Run Fable II Setup again on this PC: it builds the version for processors without AVX2."
                    .to_string())
            }
            (None, None) => return Err("launch.txt names no game executable".to_string()),
        };
        let cwd = cwd.unwrap_or_else(|| root.clone());
        Ok(Self {
            root,
            exe,
            cwd,
            args,
            env,
            out,
            err,
            settings_only: false,
        })
    }

    /// `--settings-only --cwd <dir> --exe <exe>` (dev scripts).
    fn from_args(args: &[String]) -> Result<Self, String> {
        let get = |name: &str| {
            args.iter()
                .position(|a| a == name)
                .and_then(|i| args.get(i + 1))
                .map(|v| PathBuf::from(v.trim_end_matches(['\\', '/'])))
        };
        let cwd = match get("--cwd") {
            Some(d) => d,
            None => std::env::current_dir().map_err(|e| e.to_string())?,
        };
        let exe = get("--exe").unwrap_or_default();
        let root = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(Path::to_path_buf))
            .unwrap_or_else(|| cwd.clone());
        Ok(Self {
            root,
            exe,
            cwd,
            args: Vec::new(),
            env: Vec::new(),
            out: None,
            err: None,
            settings_only: true,
        })
    }

    /// `native/fable2.toml` of the checkout the game runs from: the game finds it the same way,
    /// walking up from its own folder to the one that holds `extracted/default.xex`.
    fn settings_path(&self) -> PathBuf {
        let starts = [self.exe.parent().map(Path::to_path_buf), Some(self.cwd.clone()), Some(self.root.clone())];
        for s in starts.into_iter().flatten() {
            let mut d: Option<&Path> = Some(&s);
            for _ in 0..8 {
                let Some(x) = d else { break };
                if x.join("extracted").join("default.xex").is_file() {
                    return x.join("native").join("fable2.toml");
                }
                d = x.parent();
            }
        }
        self.root.join("native").join("fable2.toml")
    }

    fn start(&self) -> Result<(), String> {
        if !self.exe.is_file() {
            return Err(format!(
                "{} is missing.\n\nRun Fable II Setup again to rebuild the game.",
                self.exe.display()
            ));
        }
        let mut c = Command::new(&self.exe);
        c.args(&self.args)
            .current_dir(&self.cwd)
            .envs(self.env.iter().cloned())
            .stdin(Stdio::null());
        let open = |name: &Option<String>| -> Stdio {
            match name
                .as_deref()
                .map(|n| resolve(&self.cwd, n))
                .and_then(|p| std::fs::File::create(p).ok())
            {
                Some(f) => Stdio::from(f),
                None => Stdio::null(),
            }
        };
        c.stdout(open(&self.out)).stderr(open(&self.err));
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        c.spawn()
            .map_err(|e| format!("Could not start {}: {e}", self.exe.display()))?;
        Ok(())
    }
}

// ---- updates (see the top of the file) ------------------------------------------------------

/// Result of the background update check: (files, bytes) when GitHub has something newer.
#[cfg_attr(not(windows), allow(dead_code))]
static UPDATE: std::sync::Mutex<Option<(u64, u64)>> = std::sync::Mutex::new(None);

impl Launch {
    /// Only a real installation updates itself: it has the installer exe next to it and the
    /// install state. A development tree (the maintainer's checkout) is never touched.
    #[cfg_attr(not(windows), allow(dead_code))]
    fn can_update(&self) -> bool {
        let ext = if cfg!(windows) { ".exe" } else { "" };
        !self.settings_only
            && std::env::var("FABLE2_NO_UPDATE_CHECK").map(|v| v.trim() != "1").unwrap_or(true)
            && self.root.join("installer").join(format!("fable2-installer{ext}")).is_file()
            && self.root.join(".installer").join("state").is_file()
    }

    /// Runs `Update Fable II.cmd` (or the installer's `update` directly) in its own console window.
    #[cfg_attr(not(windows), allow(dead_code))]
    fn start_update(&self) -> Result<(), String> {
        let script = self.root.join("Update Fable II.cmd");
        let mut c = if script.is_file() {
            let mut c = Command::new("cmd.exe");
            c.arg("/c").arg(&script);
            c
        } else {
            let ext = if cfg!(windows) { ".exe" } else { "" };
            let mut c = Command::new(self.root.join("installer").join(format!("fable2-installer{ext}")));
            c.arg("update");
            c
        };
        c.current_dir(&self.root);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0000_0010); // CREATE_NEW_CONSOLE: the player sees the progress
        }
        c.spawn()
            .map(|_| ())
            .map_err(|e| format!("Could not start the update: {e}"))
    }
}

// ---- the settings: cvars in native/fable2.toml (native/src/pc_settings.cpp has the in-game menu) --

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Bool,
    Int,
    Float,
    Str,
}

struct Row {
    key: &'static str,
    label: &'static str,
    #[allow(dead_code)] // kept with each row for documentation; the dialog has no tooltips
    hint: &'static str,
    kind: Kind,
    vals: &'static [&'static str],
    names: &'static [&'static str],
    /// Shown when fable2.toml does not set the key: the cvar's built-in default.
    fallback: usize,
}

const DISPLAY: &[Row] = &[
    Row {
        key: "fullscreen",
        label: "Display Mode",
        hint: "Also switchable in game with F11.",
        kind: Kind::Bool,
        vals: &["false", "true"],
        names: &["Windowed", "Fullscreen"],
        fallback: 1,
    },
    Row {
        key: "vsync",
        label: "VSync",
        hint: "Synchronise frames with the monitor's refresh rate.",
        kind: Kind::Bool,
        vals: &["true", "false"],
        names: &["On", "Off"],
        fallback: 0,
    },
    Row {
        key: "fable2_frame_rate",
        label: "Frame Rate",
        hint: "Above 30 FPS some animations and physics may run differently.",
        kind: Kind::Int,
        vals: &["30", "60", "120"],
        names: &["30 FPS (original)", "60 FPS", "120 FPS"],
        fallback: 0,
    },
    Row {
        key: "resolution_scale",
        label: "Render Resolution",
        hint: "Internal resolution; 2x is much sharper and much heavier on the GPU.",
        kind: Kind::Int,
        vals: &["1", "2", "3"],
        names: &["Native (1x)", "2x (sharper)", "3x (very demanding)"],
        fallback: 0,
    },
];

const QUALITY: &[Row] = &[
    Row {
        key: "anisotropic_override",
        label: "Texture Filtering",
        hint: "",
        kind: Kind::Int,
        vals: &["0", "2", "3", "4", "5"],
        names: &["Off", "2x Anisotropic", "4x Anisotropic", "8x Anisotropic", "16x Anisotropic"],
        fallback: 2,
    },
    Row {
        key: "swap_post_effect",
        label: "Anti-Aliasing (FXAA)",
        hint: "Smooths edges; can soften fine detail and text.",
        kind: Kind::Str,
        vals: &["none", "fxaa", "fxaa_extreme"],
        names: &["Off", "FXAA", "FXAA (strong)"],
        fallback: 0,
    },
    Row {
        key: "fable2_disable_msaa",
        label: "Game MSAA",
        hint: "The game's own multisampling; off is faster.",
        kind: Kind::Bool,
        vals: &["false", "true"],
        names: &["On (original)", "Off (faster)"],
        fallback: 0,
    },
];

const CONTROLS: &[Row] = &[
    Row {
        key: "mnk_mode",
        label: "Input",
        hint: "A controller always works; this turns the keyboard and mouse on or off.",
        kind: Kind::Bool,
        vals: &["true", "false"],
        names: &["Keyboard & Mouse", "Controller only"],
        fallback: 1,
    },
    Row {
        key: "fable2_mouse_sensitivity",
        label: "Mouse Sensitivity",
        hint: "",
        kind: Kind::Float,
        vals: &["0.25", "0.5", "0.75", "1.0", "1.25", "1.5", "2.0", "3.0"],
        names: &["0.25x", "0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x", "3.0x"],
        fallback: 3,
    },
    Row {
        key: "fable2_mouse_y_scale",
        label: "Vertical Sensitivity",
        hint: "",
        kind: Kind::Float,
        vals: &["0.5", "0.75", "1.0", "1.5", "2.0"],
        names: &["0.5x", "0.75x", "1.0x", "1.5x", "2.0x"],
        fallback: 2,
    },
    Row {
        key: "fable2_mouse_invert_y",
        label: "Invert Vertical Look",
        hint: "",
        kind: Kind::Bool,
        vals: &["false", "true"],
        names: &["Off", "On"],
        fallback: 0,
    },
    Row {
        key: "fable2_lock_cursor",
        label: "Mouse Cursor",
        hint: "Keeps the cursor inside the window while playing (Alt+Tab frees it).",
        kind: Kind::Bool,
        vals: &["true", "false"],
        names: &["Locked to the game", "Free"],
        fallback: 0,
    },
];

impl Row {
    fn find(&self, v: &str) -> Option<usize> {
        let v = v.trim();
        if v.is_empty() {
            return None;
        }
        self.vals.iter().position(|x| x.eq_ignore_ascii_case(v)).or_else(|| {
            if matches!(self.kind, Kind::Float | Kind::Int) {
                let f: f64 = v.parse().ok()?;
                self.vals
                    .iter()
                    .position(|x| (x.parse::<f64>().unwrap_or(-9.0) - f).abs() < 0.001)
            } else {
                None
            }
        })
    }

    /// The value as the game's cvar file writes it.
    fn toml_value(&self, i: usize) -> String {
        let v = self.vals[i.min(self.vals.len() - 1)];
        match self.kind {
            Kind::Bool | Kind::Int => v.to_string(),
            Kind::Float => format!("{:.6}", v.parse::<f64>().unwrap_or(0.0)),
            Kind::Str => format!("\"{v}\""),
        }
    }
}

// ---- fable2.toml: `key = value` lines (the runtime's cvar file) ---------------------------------

/// `key = value` -> (key, value without quotes). Comments and tables are skipped.
fn parse_line(line: &str) -> Option<(String, String)> {
    let t = line.trim();
    if t.is_empty() || t.starts_with('#') || t.starts_with('[') {
        return None;
    }
    let (k, v) = t.split_once('=')?;
    let k = k.trim().trim_matches('"').to_string();
    let v = v.trim();
    let v = if let Some(rest) = v.strip_prefix('"') {
        rest.split('"').next().unwrap_or("").to_string()
    } else {
        v.split('#').next().unwrap_or("").trim().to_string()
    };
    if k.is_empty() {
        None
    } else {
        Some((k, v))
    }
}

/// Set `key` in the file text, keeping every other line (and comment) as it is.
fn set_value(text: &str, key: &str, value: &str) -> String {
    let mut out = String::new();
    let mut done = false;
    for line in text.lines() {
        if !done && parse_line(line).map(|(k, _)| k == key).unwrap_or(false) {
            out.push_str(&format!("{key} = {value}\n"));
            done = true;
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    if !done {
        out.push_str(&format!("{key} = {value}\n"));
    }
    out
}

// ---- settings model (shared by the window and the tests) ----------------------------------------

struct State {
    launch: Launch,
    /// The file as it is on disk (all keys, incl. ones the window doesn't show).
    text: String,
    ini: BTreeMap<String, String>,
    /// What the game would use right now for each shown key (index into the row's values).
    effective: BTreeMap<&'static str, usize>,
    cur: BTreeMap<&'static str, usize>,
}

impl State {
    fn new(launch: Launch) -> Self {
        let text = std::fs::read_to_string(launch.settings_path()).unwrap_or_default();
        let ini = text.lines().filter_map(parse_line).collect();
        let mut s = Self {
            launch,
            text,
            ini,
            effective: BTreeMap::new(),
            cur: BTreeMap::new(),
        };
        s.sync_rows();
        s
    }

    /// Groups of rows, in display order (a separator line goes between groups).
    fn groups(&self) -> Vec<Vec<&'static Row>> {
        vec![
            DISPLAY.iter().collect(),
            QUALITY.iter().collect(),
            CONTROLS.iter().collect(),
        ]
    }

    fn default_index(&self, r: &Row) -> usize {
        r.fallback
    }

    /// Settings files from before fable2_frame_rate only have the fable2_60fps switch.
    fn legacy_index(&self, r: &Row) -> Option<usize> {
        if r.key != "fable2_frame_rate" {
            return None;
        }
        let on = self.ini.get("fable2_60fps")?.trim().eq_ignore_ascii_case("true");
        Some(if on { 1 } else { 0 })
    }

    fn sync_rows(&mut self) {
        for r in self.groups().into_iter().flatten() {
            if self.effective.contains_key(r.key) {
                continue;
            }
            let k = self
                .ini
                .get(r.key)
                .and_then(|v| r.find(v))
                .or_else(|| self.legacy_index(r))
                .unwrap_or_else(|| self.default_index(r));
            self.effective.insert(r.key, k);
            self.cur.insert(r.key, k);
        }
    }

    fn get(&self, r: &Row) -> usize {
        self.cur
            .get(r.key)
            .copied()
            .unwrap_or(r.fallback)
            .min(r.vals.len() - 1)
    }

    fn dirty(&self) -> bool {
        self.cur
            .iter()
            .any(|(k, v)| self.effective.get(k) != Some(v))
    }

    fn save(&mut self) -> Result<(), String> {
        let rows: Vec<&'static Row> = self.groups().into_iter().flatten().collect();
        // Re-read first: the game may have written the file since the window opened.
        let mut text = std::fs::read_to_string(self.launch.settings_path()).unwrap_or_else(|_| self.text.clone());
        if text.trim().is_empty() {
            text = String::from("# Fable II PC settings (written by the launcher and the in-game PC Settings menu).\n");
        }
        for (k, &i) in &self.cur {
            if let Some(r) = rows.iter().find(|r| r.key == *k) {
                // Only keys the player changed (or that the file has) are written, so the game's
                // own defaults keep applying to everything else.
                let in_file = self.ini.contains_key(*k);
                if in_file || self.effective.get(k) != Some(&i) {
                    text = set_value(&text, k, &r.toml_value(i));
                    if *k == "fable2_frame_rate" {
                        // Keep the old switch consistent for builds that only know it.
                        text = set_value(&text, "fable2_60fps", if i >= 1 { "true" } else { "false" });
                    }
                }
            }
        }
        let path = self.launch.settings_path();
        if let Some(d) = path.parent() {
            let _ = std::fs::create_dir_all(d);
        }
        std::fs::write(&path, &text)
            .map_err(|e| format!("Could not save {}: {e}", path.display()))?;
        self.ini = text.lines().filter_map(parse_line).collect();
        self.text = text;
        self.effective = self.cur.clone();
        Ok(())
    }

    fn restore_defaults(&mut self) {
        for r in self.groups().into_iter().flatten() {
            let d = self.default_index(r);
            self.cur.insert(r.key, d);
        }
    }

    /// Play: save if needed, then start the game (or, `--settings-only`, just report success).
    fn play(&mut self) -> Result<(), String> {
        if self.dirty() {
            self.save()?;
        }
        if self.launch.settings_only {
            Ok(())
        } else {
            self.launch.start()
        }
    }
}

// ---- the window: a plain native Windows dialog (real controls, ClearType, per-monitor DPI) -------

#[cfg(windows)]
mod win {
    use super::*;
    use std::cell::RefCell;
    use std::ffi::c_void;
    use windows_sys::Win32::Foundation::*;
    use windows_sys::Win32::Graphics::Gdi::*;
    use windows_sys::Win32::System::ApplicationInstallationAndServicing::{
        ActivateActCtx, CreateActCtxW, ACTCTXW,
    };
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::Controls::{
        InitCommonControlsEx, ICC_STANDARD_CLASSES, INITCOMMONCONTROLSEX,
    };
    use windows_sys::Win32::UI::HiDpi::{
        AdjustWindowRectExForDpi, GetDpiForSystem, GetDpiForWindow, SetProcessDpiAwarenessContext,
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        EnableWindow, SetFocus, TrackMouseEvent, TME_LEAVE, TRACKMOUSEEVENT,
    };
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    // Message / style numbers spelled out so they don't depend on which module a crate version keeps them in.
    const WM_MOUSELEAVE_: u32 = 0x02A3;
    const WM_DPICHANGED_: u32 = 0x02E0;
    const CB_SETMINVISIBLE_: u32 = 0x1701;
    const CBN_SELCHANGE_: u32 = 1;
    const BN_CLICKED_: u32 = 0;
    const ID_PLAY: usize = 1; // IDOK: Enter
    const ID_CANCEL: usize = 2; // IDCANCEL: Esc
    const ID_RESTORE: usize = 10;
    const ID_SAVE: usize = 11;
    const ID_UPDATE: usize = 12;
    const WM_UPDATE_FOUND: u32 = 0x8000 + 1; // WM_APP + 1: the background check found an update
    const ID_COMBO0: usize = 100;

    // Layout at 96 dpi (100 %); everything is scaled by the window's dpi.
    const CW: i32 = 420; // client width
    const M: i32 = 9; // outer margin
    const BANNER_H: i32 = 76;
    const ROW_H: i32 = 27;
    const SEP_H: i32 = 9;
    const LIST_PAD: i32 = 4;
    const COMBO_W: i32 = 190;
    const BTN_H: i32 = 25;
    const LINK_H: i32 = 16;

    const fn rgb(r: u8, g: u8, b: u8) -> COLORREF {
        r as u32 | (g as u32) << 8 | (b as u32) << 16
    }
    // Midnight blue and the Guild's old gold for the title box; a parchment list.
    const C_BANNER: COLORREF = rgb(0x10, 0x15, 0x28);
    const C_BANNER_EDGE: COLORREF = rgb(0x8A, 0x6A, 0x2A);
    const C_GOLD: COLORREF = rgb(0xD9, 0xB8, 0x6C);
    const C_GOLD_DIM: COLORREF = rgb(0x8A, 0x6A, 0x2A);
    const C_PARCHMENT: COLORREF = rgb(0xFB, 0xF6, 0xEA);
    const C_INK: COLORREF = rgb(0x2B, 0x22, 0x14);
    const C_FRAME: COLORREF = rgb(0xC9, 0xB9, 0x92);
    const C_RULE: COLORREF = rgb(0xD8, 0xCB, 0xA8);
    const C_LINK: COLORREF = rgb(0x6D, 0x6D, 0x6D);
    const C_LINK_HOT: COLORREF = rgb(0x8A, 0x64, 0x20);

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(Some(0)).collect()
    }

    struct Fonts {
        ui: HFONT,
        title: HFONT,
        link: HFONT,
        link_hot: HFONT,
    }

    impl Fonts {
        fn new(dpi: u32) -> Self {
            let mk = |face: &str, px: i32, weight: i32, underline: bool| -> HFONT {
                // SAFETY: LOGFONTW is plain data; zeroed is a valid starting point.
                let mut lf: LOGFONTW = unsafe { std::mem::zeroed() };
                lf.lfHeight = -scale(px, dpi);
                lf.lfWeight = weight;
                lf.lfUnderline = underline as _;
                lf.lfCharSet = DEFAULT_CHARSET as _;
                lf.lfQuality = CLEARTYPE_QUALITY as _;
                for (i, c) in face.encode_utf16().take(31).enumerate() {
                    lf.lfFaceName[i] = c;
                }
                // SAFETY: valid LOGFONTW.
                unsafe { CreateFontIndirectW(&lf) }
            };
            Self {
                ui: mk("Segoe UI", 12, 400, false),
                title: mk("Palatino Linotype", 21, 700, false),
                link: mk("Segoe UI", 11, 400, false),
                link_hot: mk("Segoe UI", 11, 400, true),
            }
        }
        fn free(&self) {
            // SAFETY: fonts we created, not selected into any DC any more.
            unsafe {
                for f in [self.ui, self.title, self.link, self.link_hot] {
                    DeleteObject(f as _);
                }
            }
        }
    }

    fn scale(v: i32, dpi: u32) -> i32 {
        ((v as i64 * dpi as i64 + 48) / 96) as i32
    }

    struct Combo {
        hwnd: HWND,
        row: &'static Row,
    }

    struct Win {
        hwnd: HWND,
        state: State,
        dpi: u32,
        fonts: Fonts,
        combos: Vec<Combo>,
        /// Label rows (y range at current dpi) and separator lines, for painting.
        labels: Vec<(i32, &'static str)>,
        seps: Vec<i32>,
        list: RECT,
        btn_restore: HWND,
        btn_save: HWND,
        btn_play: HWND,
        btn_update: HWND,
        link: RECT,
        link_w: i32,
        link_hot: bool,
        played: bool,
    }

    thread_local! {
        static APP: RefCell<Option<Win>> = const { RefCell::new(None) };
    }

    enum After {
        Nothing,
        Fail(String),
        Close,
        /// Ask whether to update (outside the state borrow: the question box is modal).
        AskUpdate,
    }

    impl Win {
        fn s(&self, v: i32) -> i32 {
            scale(v, self.dpi)
        }

        fn child(&self, class: &str, text: &str, style: u32, id: usize) -> HWND {
            let (c, t) = (w(class), w(text));
            // SAFETY: plain child window creation; hwnd is our live top-level window.
            let h = unsafe {
                CreateWindowExW(
                    WS_EX_NOPARENTNOTIFY,
                    c.as_ptr(),
                    t.as_ptr(),
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                    0,
                    0,
                    10,
                    10,
                    self.hwnd,
                    id as _,
                    GetModuleHandleW(std::ptr::null()),
                    std::ptr::null(),
                )
            };
            // SAFETY: valid window + font.
            unsafe { SendMessageW(h, WM_SETFONT, self.fonts.ui as WPARAM, 0) };
            h
        }

        /// (Re)create the drop-downs for the current set of rows.
        fn build_combos(&mut self) {
            for c in self.combos.drain(..) {
                // SAFETY: our child window.
                unsafe { DestroyWindow(c.hwnd) };
            }
            let rows: Vec<&'static Row> = self.state.groups().into_iter().flatten().collect();
            for (i, r) in rows.into_iter().enumerate() {
                let h = self.child(
                    "COMBOBOX",
                    "",
                    CBS_DROPDOWNLIST as u32 | WS_VSCROLL,
                    ID_COMBO0 + i,
                );
                for n in r.names {
                    let t = w(n);
                    // SAFETY: NUL-terminated string, copied by the control.
                    unsafe { SendMessageW(h, CB_ADDSTRING, 0, t.as_ptr() as LPARAM) };
                }
                // SAFETY: plain control messages; the height given here is field + open list.
                unsafe {
                    SetWindowPos(h, std::ptr::null_mut(), 0, 0, self.s(COMBO_W), self.s(300), SWP_NOZORDER | SWP_NOACTIVATE);
                    SendMessageW(h, CB_SETMINVISIBLE_, 12, 0);
                    SendMessageW(h, CB_SETCURSEL, self.state.get(r) as WPARAM, 0);
                }
                self.combos.push(Combo { hwnd: h, row: r });
            }
            // keep tab order: combos first, then the buttons
            for b in [self.btn_restore, self.btn_update, self.btn_save, self.btn_play] {
                // SAFETY: our child windows.
                unsafe { SetWindowPos(b, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) };
            }
        }

        fn refresh_values(&self) {
            for c in &self.combos {
                // SAFETY: plain control message.
                unsafe { SendMessageW(c.hwnd, CB_SETCURSEL, self.state.get(c.row) as WPARAM, 0) };
            }
            self.refresh_save();
        }

        fn refresh_save(&self) {
            // SAFETY: our child window.
            unsafe { EnableWindow(self.btn_save, self.state.dirty() as BOOL) };
        }

        /// Position everything for the current dpi and size the window to fit. `at`: top-left to use.
        fn layout(&mut self, at: Option<(i32, i32)>) {
            let dpi = self.dpi;
            let s = move |v| scale(v, dpi);
            let cw = s(CW);
            let mut y = s(M) + s(BANNER_H) + s(M);
            let list_top = y;
            y += s(LIST_PAD);
            self.labels.clear();
            self.seps.clear();
            let groups = self.state.groups();
            let n = groups.len();
            let mut ci = 0;
            let combo_w = s(COMBO_W);
            let combo_x = cw - s(M) - s(8) - combo_w;
            for (gi, rows) in groups.into_iter().enumerate() {
                for r in rows {
                    let row_h = s(ROW_H);
                    self.labels.push((y + row_h / 2, r.label));
                    if let Some(c) = self.combos.get(ci) {
                        let mut rc: RECT = RECT { left: 0, top: 0, right: 0, bottom: 0 };
                        // SAFETY: our child window; height = closed field + drop list.
                        unsafe {
                            GetWindowRect(c.hwnd, &mut rc);
                            let field_h = rc.bottom - rc.top;
                            SetWindowPos(
                                c.hwnd,
                                std::ptr::null_mut(),
                                combo_x,
                                y + (row_h - field_h) / 2,
                                combo_w,
                                s(300),
                                SWP_NOZORDER | SWP_NOACTIVATE,
                            );
                        }
                    }
                    ci += 1;
                    y += row_h;
                }
                if gi + 1 < n {
                    self.seps.push(y + s(SEP_H) / 2);
                    y += s(SEP_H);
                }
            }
            y += s(LIST_PAD);
            self.list = RECT { left: s(M), top: list_top, right: cw - s(M), bottom: y };
            y += s(M);
            let bh = s(BTN_H);
            // SAFETY: our child windows.
            unsafe {
                let place = |h: HWND, x: i32, wd: i32| {
                    SetWindowPos(h, std::ptr::null_mut(), x, y, wd, bh, SWP_NOZORDER | SWP_NOACTIVATE);
                };
                place(self.btn_restore, s(M), s(128));
                place(self.btn_update, s(M) + s(128) + s(6), s(90));
                place(self.btn_play, cw - s(M) - s(80), s(80));
                place(self.btn_save, cw - s(M) - s(80) - s(6) - s(80), s(80));
            }
            y += bh + s(5);
            self.link = RECT { left: 0, top: y, right: cw, bottom: y + s(LINK_H) };
            // SAFETY: measuring text on the window's DC, released right after.
            unsafe {
                let dc = GetDC(self.hwnd);
                let old = SelectObject(dc, self.fonts.link as _);
                let t: Vec<u16> = REPO_TEXT.encode_utf16().collect();
                let mut sz = SIZE { cx: 0, cy: 0 };
                GetTextExtentPoint32W(dc, t.as_ptr(), t.len() as i32, &mut sz);
                SelectObject(dc, old);
                ReleaseDC(self.hwnd, dc);
                self.link_w = sz.cx;
            }
            y += s(LINK_H) + s(6);
            let ch = y;

            // window size from client size
            let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            let mut r = RECT { left: 0, top: 0, right: cw, bottom: ch };
            // SAFETY: valid RECT.
            unsafe { AdjustWindowRectExForDpi(&mut r, style, 0, 0, self.dpi) };
            let (ww, wh) = (r.right - r.left, r.bottom - r.top);
            // SAFETY: our window.
            unsafe {
                match at {
                    Some((x, y)) => SetWindowPos(self.hwnd, std::ptr::null_mut(), x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE),
                    None => SetWindowPos(self.hwnd, std::ptr::null_mut(), 0, 0, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE),
                };
                InvalidateRect(self.hwnd, std::ptr::null(), 1);
            }
        }

        fn set_fonts(&mut self) {
            let old = std::mem::replace(&mut self.fonts, Fonts::new(self.dpi));
            for h in self
                .combos
                .iter()
                .map(|c| c.hwnd)
                .chain([self.btn_restore, self.btn_update, self.btn_save, self.btn_play])
            {
                // SAFETY: our child windows; redraw with the new font.
                unsafe { SendMessageW(h, WM_SETFONT, self.fonts.ui as WPARAM, 1) };
            }
            old.free();
        }

        fn paint(&self, dc: HDC) {
            // SAFETY: GDI drawing on the paint DC; every object we create is deleted again.
            unsafe {
                let s = |v| self.s(v);
                SetBkMode(dc, TRANSPARENT as _);
                let fill = |r: &RECT, c: COLORREF| {
                    let b = CreateSolidBrush(c);
                    FillRect(dc, r, b);
                    DeleteObject(b as _);
                };
                let frame = |r: &RECT, c: COLORREF| {
                    let b = CreateSolidBrush(c);
                    FrameRect(dc, r, b);
                    DeleteObject(b as _);
                };
                let cw = s(CW);

                // ---- title box
                let banner = RECT { left: s(M), top: s(M), right: cw - s(M), bottom: s(M) + s(BANNER_H) };
                fill(&banner, C_BANNER);
                frame(&banner, C_BANNER_EDGE);
                let old_font = SelectObject(dc, self.fonts.title as _);
                let words = ["FABLE", "II", "REBORN"];
                let track = s(5);
                let gap = s(13);
                let mut glyphs: Vec<(Vec<u16>, i32, i32)> = Vec::new(); // (char, width, extra gap before)
                for (wi, word) in words.iter().enumerate() {
                    for (ci, ch) in word.encode_utf16().enumerate() {
                        let mut sz = SIZE { cx: 0, cy: 0 };
                        GetTextExtentPoint32W(dc, &ch, 1, &mut sz);
                        glyphs.push((vec![ch], sz.cx, if ci == 0 && wi > 0 { gap } else { 0 }));
                    }
                }
                let text_w: i32 = glyphs.iter().map(|(_, w, g)| w + g).sum::<i32>()
                    + track * (glyphs.len() as i32 - 1);
                let mut tm: TEXTMETRICW = std::mem::zeroed();
                GetTextMetricsW(dc, &mut tm);
                let cy = (banner.top + banner.bottom) / 2;
                let plate_h = s(36);
                let plate = RECT {
                    left: cw / 2 - text_w / 2 - s(18),
                    top: cy - plate_h / 2,
                    right: cw / 2 - text_w / 2 - s(18) + text_w + s(36),
                    bottom: cy - plate_h / 2 + plate_h,
                };
                // gold rules either side of the title, each ending in a small diamond
                let rule_h = s(1).max(1);
                fill(&RECT { left: banner.left + s(22), top: cy - rule_h / 2, right: plate.left, bottom: cy - rule_h / 2 + rule_h }, C_GOLD_DIM);
                fill(&RECT { left: plate.right, top: cy - rule_h / 2, right: banner.right - s(22), bottom: cy - rule_h / 2 + rule_h }, C_GOLD_DIM);
                let diamond = |x: i32| {
                    let r = s(4);
                    let pts = [
                        POINT { x, y: cy - r },
                        POINT { x: x + r, y: cy },
                        POINT { x, y: cy + r },
                        POINT { x: x - r, y: cy },
                    ];
                    let b = CreateSolidBrush(C_GOLD);
                    let p = CreatePen(PS_SOLID, 1, C_GOLD);
                    let ob = SelectObject(dc, b as _);
                    let op = SelectObject(dc, p as _);
                    Polygon(dc, pts.as_ptr(), 4);
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                    DeleteObject(b as _);
                    DeleteObject(p as _);
                };
                diamond(banner.left + s(18));
                diamond(banner.right - s(18));
                SetTextColor(dc, C_GOLD);
                let mut x = cw / 2 - text_w / 2;
                let ty = cy - tm.tmHeight / 2;
                for (ch, wd, g) in &glyphs {
                    x += g;
                    TextOutW(dc, x, ty, ch.as_ptr(), 1);
                    x += wd + track;
                }

                // ---- settings list
                fill(&self.list, C_PARCHMENT);
                frame(&self.list, C_FRAME);
                SelectObject(dc, self.fonts.ui as _);
                SetTextColor(dc, C_INK);
                for (cy, label) in &self.labels {
                    let t = w(label);
                    let mut r = RECT { left: self.list.left + s(10), top: cy - s(ROW_H) / 2, right: cw / 2, bottom: cy + s(ROW_H) / 2 };
                    DrawTextW(dc, t.as_ptr(), -1, &mut r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
                }
                for y in &self.seps {
                    fill(&RECT { left: self.list.left + 1, top: *y, right: self.list.right - 1, bottom: y + 1 }, C_RULE);
                }

                // ---- official source link
                let hot = self.link_hot && !REPO_URL.is_empty();
                SelectObject(dc, if hot { self.fonts.link_hot } else { self.fonts.link } as _);
                SetTextColor(dc, if hot { C_LINK_HOT } else { C_LINK });
                let t = w(REPO_TEXT);
                let mut r = self.link;
                DrawTextW(dc, t.as_ptr(), -1, &mut r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                SelectObject(dc, old_font);
            }
        }

        /// Clickable part of the link line (just the text).
        fn link_hit(&self, x: i32, y: i32) -> bool {
            if REPO_URL.is_empty() {
                return false;
            }
            let text_w = self.link_w;
            let cx = (self.link.left + self.link.right) / 2;
            y >= self.link.top && y < self.link.bottom && x >= cx - text_w / 2 && x < cx + text_w / 2
        }

        fn command(&mut self, id: usize, code: u32) -> After {
            match id {
                ID_PLAY if code == BN_CLICKED_ => match self.state.play() {
                    Ok(()) => {
                        self.played = true;
                        After::Close
                    }
                    Err(e) => After::Fail(e),
                },
                ID_CANCEL => After::Close,
                ID_UPDATE if code == BN_CLICKED_ => After::AskUpdate,
                ID_RESTORE if code == BN_CLICKED_ => {
                    self.state.restore_defaults();
                    self.refresh_values();
                    After::Nothing
                }
                ID_SAVE if code == BN_CLICKED_ => {
                    let r = self.state.save();
                    self.refresh_save();
                    match r {
                        Ok(()) => After::Nothing,
                        Err(e) => After::Fail(e),
                    }
                }
                i if i >= ID_COMBO0 && code == CBN_SELCHANGE_ => {
                    if let Some(c) = self.combos.get(i - ID_COMBO0) {
                        // SAFETY: plain control message.
                        let sel = unsafe { SendMessageW(c.hwnd, CB_GETCURSEL, 0, 0) };
                        if sel >= 0 {
                            self.state.cur.insert(c.row.key, sel as usize);
                        }
                    }
                    self.refresh_save();
                    After::Nothing
                }
                _ => After::Nothing,
            }
        }
    }

    /// Yes/No question box; true on Yes.
    fn ask(hwnd: HWND, text: &str) -> bool {
        let (t, c) = (w(text), w("Fable II Reborn"));
        // SAFETY: NUL-terminated strings; MB_YESNO | MB_ICONQUESTION.
        unsafe { MessageBoxW(hwnd, t.as_ptr(), c.as_ptr(), MB_YESNO | MB_ICONQUESTION) == IDYES }
    }

    fn lo(v: usize) -> u32 {
        (v & 0xFFFF) as u32
    }
    fn hi(v: usize) -> u32 {
        ((v >> 16) & 0xFFFF) as u32
    }
    fn mouse_xy(l: LPARAM) -> (i32, i32) {
        ((l & 0xFFFF) as i16 as i32, ((l >> 16) & 0xFFFF) as i16 as i32)
    }

    unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
        match msg {
            WM_CLOSE => {
                DestroyWindow(hwnd);
                return 0;
            }
            WM_DESTROY => {
                PostQuitMessage(0);
                return 0;
            }
            WM_ERASEBKGND => {} // default: COLOR_BTNFACE from the class brush
            _ => {}
        }
        // Everything else needs the state; never hold it across a modal call (message box).
        let after = APP.with(|a| {
            let Ok(mut g) = a.try_borrow_mut() else {
                return None;
            };
            let app = g.as_mut()?;
            if app.hwnd != hwnd {
                return None;
            }
            match msg {
                WM_PAINT => {
                    let mut ps: PAINTSTRUCT = std::mem::zeroed();
                    let dc = BeginPaint(hwnd, &mut ps);
                    app.paint(dc);
                    EndPaint(hwnd, &ps);
                    Some((0, After::Nothing))
                }
                WM_COMMAND => Some((0, app.command(lo(wp) as usize, hi(wp)))),
                WM_MOUSEMOVE => {
                    let (x, y) = mouse_xy(lp);
                    let hot = app.link_hit(x, y);
                    if hot != app.link_hot {
                        app.link_hot = hot;
                        InvalidateRect(hwnd, &app.link, 0);
                    }
                    let mut t = TRACKMOUSEEVENT {
                        cbSize: std::mem::size_of::<TRACKMOUSEEVENT>() as u32,
                        dwFlags: TME_LEAVE,
                        hwndTrack: hwnd,
                        dwHoverTime: 0,
                    };
                    TrackMouseEvent(&mut t);
                    Some((0, After::Nothing))
                }
                WM_MOUSELEAVE_ => {
                    if app.link_hot {
                        app.link_hot = false;
                        InvalidateRect(hwnd, &app.link, 0);
                    }
                    Some((0, After::Nothing))
                }
                WM_SETCURSOR if app.link_hot && lo(lp as usize) == HTCLIENT => {
                    SetCursor(LoadCursorW(std::ptr::null_mut(), IDC_HAND));
                    Some((1, After::Nothing))
                }
                WM_LBUTTONUP => {
                    let (x, y) = mouse_xy(lp);
                    if app.link_hit(x, y) {
                        let (op, url) = (w("open"), w(REPO_URL));
                        ShellExecuteW(hwnd, op.as_ptr(), url.as_ptr(), std::ptr::null(), std::ptr::null(), SW_SHOWNORMAL);
                    }
                    Some((0, After::Nothing))
                }
                WM_UPDATE_FOUND => {
                    ShowWindow(app.btn_update, SW_SHOW);
                    let t = w("Fable II Reborn - Options  (update available)");
                    SetWindowTextW(hwnd, t.as_ptr());
                    Some((0, After::Nothing))
                }
                WM_DPICHANGED_ => {
                    app.dpi = hi(wp).max(96);
                    app.set_fonts();
                    let r = &*(lp as *const RECT);
                    app.layout(Some((r.left, r.top)));
                    Some((0, After::Nothing))
                }
                _ => None,
            }
        });
        match after {
            Some((ret, After::Nothing)) => ret,
            Some((ret, After::Fail(e))) => {
                message(&e);
                ret
            }
            Some((ret, After::Close)) => {
                DestroyWindow(hwnd);
                ret
            }
            Some((ret, After::AskUpdate)) => {
                let (files, bytes) = UPDATE.lock().ok().and_then(|g| *g).unwrap_or((0, 0));
                let q = format!(
                    "A new version of Fable II Reborn is on GitHub ({files} changed file(s), {}).\n\n\
                     Update now? The launcher closes and the update runs in a console window: it downloads \
                     only what changed and rebuilds what that needs (a few minutes, longer if the game code \
                     has to be regenerated). Start Fable II again when it says it is done.",
                    fable2_installer::util::human_bytes(bytes)
                );
                if ask(hwnd, &q) {
                    // Save changed settings first, then hand over to the updater.
                    let r = APP.with(|a| {
                        let mut g = a.borrow_mut();
                        let app = g.as_mut().ok_or_else(|| "the window is gone".to_string())?;
                        if app.state.dirty() {
                            app.state.save()?;
                        }
                        app.state.launch.start_update()
                    });
                    match r {
                        Ok(()) => {
                            DestroyWindow(hwnd);
                        }
                        Err(e) => message(&e),
                    }
                }
                ret
            }
            None => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }

    /// Modern (Windows 10/11) control look: activate Common Controls 6 from a tiny manifest.
    fn visual_styles() {
        let manifest = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
      processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
</assembly>
"#;
        let path = std::env::temp_dir().join("fable2-launcher.manifest");
        if std::fs::read_to_string(&path).ok().as_deref() != Some(manifest)
            && std::fs::write(&path, manifest).is_err()
        {
            return;
        }
        let p = w(&path.to_string_lossy());
        // SAFETY: ACTCTXW is plain data; the path string outlives the call.
        unsafe {
            let mut ctx: ACTCTXW = std::mem::zeroed();
            ctx.cbSize = std::mem::size_of::<ACTCTXW>() as u32;
            ctx.lpSource = p.as_ptr();
            let h = CreateActCtxW(&ctx);
            if h != INVALID_HANDLE_VALUE {
                let mut cookie = 0usize;
                ActivateActCtx(h, &mut cookie);
            }
            let icc = INITCOMMONCONTROLSEX {
                dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
                dwICC: ICC_STANDARD_CLASSES,
            };
            InitCommonControlsEx(&icc);
        }
    }

    fn make_icon(n: usize) -> HICON {
        let rgba = icon_rgba(n);
        let mut bgra = rgba.clone();
        for p in bgra.chunks_exact_mut(4) {
            p.swap(0, 2);
        }
        let and = vec![0u8; n * n / 8];
        // SAFETY: buffers sized for an n x n 32-bpp icon.
        unsafe {
            CreateIcon(
                GetModuleHandleW(std::ptr::null()),
                n as i32,
                n as i32,
                1,
                32,
                and.as_ptr(),
                bgra.as_ptr(),
            )
        }
    }

    /// Shows the window; returns true when Play was pressed (and succeeded).
    pub fn run(state: State) -> bool {
        // SAFETY: plain Win32 window setup and message loop on this (the only UI) thread.
        unsafe {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            visual_styles();
            let inst = GetModuleHandleW(std::ptr::null());
            let class = w("Fable2PcLauncher");
            let wc = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(wndproc),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: inst,
                hIcon: make_icon(32),
                hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
                hbrBackground: (COLOR_BTNFACE + 1) as usize as HBRUSH,
                lpszMenuName: std::ptr::null(),
                lpszClassName: class.as_ptr(),
                hIconSm: make_icon(16),
            };
            RegisterClassExW(&wc);
            let title = w("Fable II Reborn - Options");
            let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            let hwnd = CreateWindowExW(
                WS_EX_CONTROLPARENT,
                class.as_ptr(),
                title.as_ptr(),
                style,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                100,
                100,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                inst,
                std::ptr::null(),
            );
            if hwnd.is_null() {
                return state.launch.settings_only;
            }
            let mut dpi = GetDpiForWindow(hwnd);
            if dpi == 0 {
                dpi = GetDpiForSystem().max(96);
            }

            let mut app = Win {
                hwnd,
                state,
                dpi,
                fonts: Fonts::new(dpi),
                combos: Vec::new(),
                labels: Vec::new(),
                seps: Vec::new(),
                list: RECT { left: 0, top: 0, right: 0, bottom: 0 },
                btn_restore: std::ptr::null_mut(),
                btn_save: std::ptr::null_mut(),
                btn_play: std::ptr::null_mut(),
                btn_update: std::ptr::null_mut(),
                link: RECT { left: 0, top: 0, right: 0, bottom: 0 },
                link_w: 0,
                link_hot: false,
                played: false,
            };
            app.btn_restore = app.child("BUTTON", "Restore to Defaults", BS_PUSHBUTTON as u32, ID_RESTORE);
            app.btn_save = app.child("BUTTON", "Save", BS_PUSHBUTTON as u32, ID_SAVE);
            app.btn_play = app.child("BUTTON", "Play", BS_DEFPUSHBUTTON as u32, ID_PLAY);
            app.btn_update = app.child("BUTTON", "Update", BS_PUSHBUTTON as u32, ID_UPDATE);
            ShowWindow(app.btn_update, SW_HIDE);
            if app.state.launch.can_update() {
                let root = app.state.launch.root.clone();
                let target = hwnd as isize;
                std::thread::spawn(move || {
                    if let Ok(u) = fable2_installer::setup::check_update(&root) {
                        if u.available {
                            if let Ok(mut g) = UPDATE.lock() {
                                *g = Some((u.files, u.bytes));
                            }
                            // SAFETY: posting to a window handle is safe even if it is gone by now.
                            unsafe { PostMessageW(target as HWND, WM_UPDATE_FOUND, 0, 0) };
                        }
                    }
                });
            }
            app.build_combos();
            app.layout(None);
            app.refresh_save();

            // centre on the work area of the window's monitor
            let mut wr = RECT { left: 0, top: 0, right: 0, bottom: 0 };
            GetWindowRect(hwnd, &mut wr);
            let mut work = RECT { left: 0, top: 0, right: 0, bottom: 0 };
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut work as *mut RECT as *mut c_void, 0);
            let (ww, wh) = (wr.right - wr.left, wr.bottom - wr.top);
            SetWindowPos(
                hwnd,
                std::ptr::null_mut(),
                work.left + (work.right - work.left - ww) / 2,
                work.top + (work.bottom - work.top - wh) / 2,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER,
            );
            let play = app.btn_play;
            APP.with(|a| *a.borrow_mut() = Some(app));

            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
            SetFocus(play);

            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
                if IsDialogMessageW(hwnd, &msg) == 0 {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            APP.with(|a| {
                let app = a.borrow_mut().take();
                app.map(|x| {
                    x.fonts.free();
                    x.played
                })
                .unwrap_or(false)
            })
        }
    }
}

/// The app icon (the "II Reborn" wolf emblem), `n` x `n` RGBA.
#[cfg_attr(not(windows), allow(dead_code))]
fn icon_rgba(n: usize) -> Vec<u8> {
    fable2_installer::brand::icon_rgba(n)
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let loaded = if argv.iter().any(|a| a == "--settings-only") {
        Launch::from_args(&argv)
    } else {
        Launch::load()
    };
    let launch = match loaded {
        Ok(l) => l,
        Err(e) => {
            message(&e);
            std::process::exit(1);
        }
    };
    if !launch.settings_only && argv.iter().any(|a| a == "--play" || a == "-play") {
        if let Err(e) = launch.start() {
            message(&e);
            std::process::exit(1);
        }
        return;
    }
    let settings_only = launch.settings_only;
    #[cfg(windows)]
    let played = win::run(State::new(launch));
    #[cfg(not(windows))]
    let played = {
        // No window off Windows (development only): behave like Play with the saved settings.
        let mut s = State::new(launch);
        match s.play() {
            Ok(()) => true,
            Err(e) => {
                message(&e);
                false
            }
        }
    };
    if settings_only && !played {
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn toml_lines() {
        assert_eq!(parse_line("vsync = true"), Some(("vsync".into(), "true".into())));
        assert_eq!(
            parse_line("swap_post_effect = \"fxaa\" # comment"),
            Some(("swap_post_effect".into(), "fxaa".into()))
        );
        assert_eq!(parse_line("# x = 1"), None);
        assert_eq!(parse_line("[section]"), None);
        let t = "# head\nvsync = true\nkeybind_a = \"F\"\n";
        let t2 = set_value(t, "vsync", "false");
        assert_eq!(t2, "# head\nvsync = false\nkeybind_a = \"F\"\n");
        let t3 = set_value(&t2, "fable2_60fps", "true");
        assert!(t3.ends_with("fable2_60fps = true\n"));
    }

    #[test]
    fn rows_format_like_the_runtime() {
        let r = &CONTROLS[2];
        assert_eq!(r.key, "fable2_mouse_y_scale");
        assert_eq!(r.find("0.750000"), Some(1));
        assert_eq!(r.toml_value(1), "0.750000");
        assert_eq!(QUALITY[1].toml_value(1), "\"fxaa\"");
        assert_eq!(DISPLAY[3].find("2"), Some(1));
        assert_eq!(DISPLAY[2].key, "fable2_frame_rate");
        assert_eq!(DISPLAY[2].find("120"), Some(2));
        assert_eq!(DISPLAY[2].toml_value(2), "120");
        for r in DISPLAY.iter().chain(QUALITY).chain(CONTROLS) {
            assert_eq!(r.vals.len(), r.names.len(), "{}", r.key);
            assert!(r.fallback < r.vals.len(), "{}", r.key);
        }
    }
}
