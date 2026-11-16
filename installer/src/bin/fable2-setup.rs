//! Fable II Setup: the point-and-click installer. Pages: Welcome, Requirements, Your game,
//! Install location, Installing, Done. The work itself is in `fable2_installer::setup`.
#![cfg_attr(windows, windows_subsystem = "windows")]

use eframe::egui::{self, Color32, RichText, Stroke};
use fable2_installer::{setup, util};
use setup::{Check, GameInfo, GameProblem, GameSource, Installer, UpdateInfo};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

// ---- look ------------------------------------------------------------------------------------

// Albion at night: midnight blue, parchment, and the Guild's old gold.
const BG: Color32 = Color32::from_rgb(0x0E, 0x12, 0x20);
const SIDE: Color32 = Color32::from_rgb(0x08, 0x0B, 0x16);
const CARD: Color32 = Color32::from_rgb(0x16, 0x1B, 0x2E);
const CARD_EDGE: Color32 = Color32::from_rgb(0x2C, 0x33, 0x52);
const TEXT: Color32 = Color32::from_rgb(0xF0, 0xE7, 0xD2);
const MUTED: Color32 = Color32::from_rgb(0xA3, 0x9C, 0x8A);
const ACCENT: Color32 = Color32::from_rgb(0x8A, 0x64, 0x20);
const ACCENT_HI: Color32 = Color32::from_rgb(0xE0, 0xB4, 0x55);
const GOLD: Color32 = Color32::from_rgb(0xD9, 0xB8, 0x6C);
const OK: Color32 = Color32::from_rgb(0x6F, 0xB0, 0x6A);
const WARN: Color32 = Color32::from_rgb(0xD9, 0xA0, 0x3A);
const BAD: Color32 = Color32::from_rgb(0xE5, 0x5A, 0x4E);

static SERIF: std::sync::OnceLock<(bool, bool)> = std::sync::OnceLock::new();

/// A book face from the Windows fonts folder for headings and the title: Palatino Linotype if
/// it is there (it suits Albion), else Georgia. Without either everything stays in egui's own font.
fn load_serif(ctx: &egui::Context) {
    let dir = std::env::var_os("WINDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Windows"))
        .join("Fonts");
    let mut fonts = egui::FontDefinitions::default();
    let base = fonts
        .families
        .get(&egui::FontFamily::Proportional)
        .cloned()
        .unwrap_or_default();
    let mut found = [false, false];
    for (k, (key, files)) in [
        ("serif", ["pala.ttf", "georgia.ttf"]),
        ("serif-bold", ["palab.ttf", "georgiab.ttf"]),
    ]
    .into_iter()
    .enumerate()
    {
        if let Some(bytes) = files.iter().find_map(|f| std::fs::read(dir.join(f)).ok()) {
            fonts
                .font_data
                .insert(key.to_owned(), egui::FontData::from_owned(bytes));
            let mut fam = vec![key.to_owned()];
            fam.extend(base.iter().cloned());
            fonts
                .families
                .insert(egui::FontFamily::Name(key.into()), fam);
            found[k] = true;
        }
    }
    let _ = SERIF.set((found[0], found[1]));
    if found[0] || found[1] {
        ctx.set_fonts(fonts);
    }
}

/// The serif family (bold if asked and available), or egui's proportional font.
fn serif(bold: bool) -> egui::FontFamily {
    let (reg, b) = SERIF.get().copied().unwrap_or((false, false));
    if bold && b {
        egui::FontFamily::Name("serif-bold".into())
    } else if reg {
        egui::FontFamily::Name("serif".into())
    } else {
        egui::FontFamily::Proportional
    }
}

fn style(ctx: &egui::Context) {
    load_serif(ctx);
    let mut s = (*ctx.style()).clone();
    use egui::{FontFamily::Proportional, FontId, TextStyle};
    s.text_styles = [
        (TextStyle::Heading, FontId::new(28.0, serif(false))),
        (TextStyle::Body, FontId::new(15.0, Proportional)),
        (TextStyle::Button, FontId::new(15.0, Proportional)),
        (TextStyle::Small, FontId::new(12.5, Proportional)),
        (
            TextStyle::Monospace,
            FontId::new(12.5, egui::FontFamily::Monospace),
        ),
    ]
    .into();
    s.spacing.item_spacing = egui::vec2(10.0, 10.0);
    s.spacing.button_padding = egui::vec2(16.0, 8.0);
    s.spacing.interact_size.y = 32.0;
    let mut v = egui::Visuals::dark();
    v.override_text_color = Some(TEXT);
    v.panel_fill = BG;
    v.window_fill = BG;
    v.extreme_bg_color = Color32::from_rgb(0x07, 0x0A, 0x14);
    v.faint_bg_color = CARD;
    v.hyperlink_color = ACCENT_HI;
    v.selection.bg_fill = ACCENT.linear_multiply(0.6);
    v.selection.stroke = Stroke::new(1.0_f32, ACCENT_HI);
    let r = egui::Rounding::same(7.0);
    for w in [
        &mut v.widgets.noninteractive,
        &mut v.widgets.inactive,
        &mut v.widgets.hovered,
        &mut v.widgets.active,
        &mut v.widgets.open,
    ] {
        w.rounding = r;
    }
    v.widgets.inactive.weak_bg_fill = Color32::from_rgb(0x1F, 0x26, 0x42);
    v.widgets.inactive.bg_fill = Color32::from_rgb(0x1F, 0x26, 0x42);
    v.widgets.inactive.bg_stroke = Stroke::new(1.0_f32, CARD_EDGE);
    v.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x29, 0x31, 0x55);
    v.widgets.hovered.bg_fill = Color32::from_rgb(0x29, 0x31, 0x55);
    v.widgets.hovered.bg_stroke = Stroke::new(1.0_f32, ACCENT.linear_multiply(0.7));
    v.widgets.active.weak_bg_fill = Color32::from_rgb(0x33, 0x3C, 0x68);
    v.widgets.noninteractive.bg_stroke = Stroke::new(1.0_f32, CARD_EDGE);
    v.widgets.active.fg_stroke = Stroke::new(2.0_f32, Color32::WHITE);
    v.widgets.noninteractive.fg_stroke = Stroke::new(1.0_f32, TEXT);
    s.visuals = v;
    // Windows in light mode would otherwise swap in egui's light theme.
    ctx.set_theme(egui::ThemePreference::Dark);
    ctx.set_style_of(egui::Theme::Dark, s.clone());
    ctx.set_style_of(egui::Theme::Light, s);
}

fn primary(text: &str) -> egui::Button<'static> {
    egui::Button::new(
        RichText::new(text)
            .color(Color32::from_rgb(0xFF, 0xF8, 0xE8))
            .strong()
            .size(15.5),
    )
    .fill(ACCENT)
    .stroke(Stroke::new(1.0_f32, ACCENT_HI.linear_multiply(0.55)))
    .min_size(egui::vec2(120.0, 38.0))
    .rounding(8.0_f32)
}

fn secondary(text: &str) -> egui::Button<'static> {
    egui::Button::new(RichText::new(text).size(15.0))
        .min_size(egui::vec2(100.0, 38.0))
        .rounding(8.0_f32)
}

fn card<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::none()
        .fill(CARD)
        .stroke(Stroke::new(1.0_f32, CARD_EDGE))
        .rounding(10.0_f32)
        .inner_margin(egui::Margin::symmetric(18.0, 14.0))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            add(ui)
        })
        .inner
}

#[derive(Clone, Copy, PartialEq)]
enum Icon {
    Ok,
    Bad,
    Warn,
    Busy,
    Dot,
}

fn icon(ui: &mut egui::Ui, kind: Icon) {
    let (rect, _) = ui.allocate_exact_size(egui::vec2(22.0, 22.0), egui::Sense::hover());
    let c = rect.center();
    let p = ui.painter();
    match kind {
        Icon::Busy => {
            egui::Spinner::new()
                .size(18.0)
                .color(ACCENT)
                .paint_at(ui, rect.shrink(2.0));
        }
        Icon::Ok => {
            p.circle_filled(c, 10.0, OK);
            let s = Stroke::new(2.4_f32, Color32::WHITE);
            p.line_segment([c + egui::vec2(-4.5, 0.5), c + egui::vec2(-1.2, 3.8)], s);
            p.line_segment([c + egui::vec2(-1.2, 3.8), c + egui::vec2(5.0, -3.5)], s);
        }
        Icon::Bad => {
            p.circle_filled(c, 10.0, BAD);
            let s = Stroke::new(2.4_f32, Color32::WHITE);
            p.line_segment([c + egui::vec2(-4.0, -4.0), c + egui::vec2(4.0, 4.0)], s);
            p.line_segment([c + egui::vec2(4.0, -4.0), c + egui::vec2(-4.0, 4.0)], s);
        }
        Icon::Warn => {
            p.circle_filled(c, 10.0, WARN);
            let s = Stroke::new(2.6_f32, Color32::from_rgb(0x20, 0x16, 0x00));
            p.line_segment([c + egui::vec2(0.0, -5.0), c + egui::vec2(0.0, 1.5)], s);
            p.circle_filled(
                c + egui::vec2(0.0, 5.0),
                1.5,
                Color32::from_rgb(0x20, 0x16, 0x00),
            );
        }
        Icon::Dot => {
            p.circle_stroke(c, 9.0, Stroke::new(1.5_f32, MUTED));
        }
    }
}

fn window_icon() -> egui::IconData {
    let n = 64usize;
    egui::IconData {
        rgba: fable2_installer::brand::icon_rgba(n),
        width: n as u32,
        height: n as u32,
    }
}

// ---- state -----------------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Page {
    Welcome,
    Requirements,
    Game,
    Location,
    Installing,
    Done,
}

const PAGES: [(Page, &str); 6] = [
    (Page::Welcome, "Welcome"),
    (Page::Requirements, "Requirements"),
    (Page::Game, "Your game"),
    (Page::Location, "Install location"),
    (Page::Installing, "Installing"),
    (Page::Done, "Play"),
];

#[derive(Clone)]
enum GameState {
    None,
    Checking,
    Ok(GameInfo),
    Problem(GameProblem),
}

#[derive(Clone)]
enum UpdState {
    Checking,
    Done(Result<UpdateInfo, String>),
}

struct Reqs {
    python: Check,
    tools: Check,
    log: Vec<String>,
}

struct App {
    page: Page,
    source: Option<setup::Source>,
    payload_bytes: u64,
    load_error: Option<String>,
    title: String,
    title_ids: Vec<String>,
    link_name: String,
    repaint: Arc<dyn Fn() + Send + Sync>,

    reqs: Arc<Mutex<Reqs>>,
    show_req_log: bool,

    game: Option<GameSource>,
    game_state: Arc<Mutex<(u64, GameState)>>,
    game_gen: u64,

    location: String,
    desktop: bool,
    start_menu: bool,
    both_cpus: bool,

    installer: Option<Installer>,
    show_log: bool,
    confirm_cancel: bool,

    /// An existing installation found at the default location (or picked), offered for updating.
    existing: Option<PathBuf>,
    upd: Arc<Mutex<(u64, UpdState)>>,
    upd_gen: u64,
    updating: bool,
    logo: Option<egui::TextureHandle>,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        style(&cc.egui_ctx);
        let ctx = cc.egui_ctx.clone();
        let repaint: Arc<dyn Fn() + Send + Sync> = Arc::new(move || ctx.request_repaint());
        let setup_exe = std::env::current_exe().unwrap_or_default();
        let mut app = App {
            page: Page::Welcome,
            source: None,
            payload_bytes: 0,
            load_error: None,
            title: "Fable II".into(),
            title_ids: vec!["4D5307F1".into()],
            link_name: "extracted".into(),
            repaint,
            reqs: Arc::new(Mutex::new(Reqs {
                python: Check::Unknown,
                tools: Check::Unknown,
                log: Vec::new(),
            })),
            show_req_log: false,
            game: None,
            game_state: Arc::new(Mutex::new((0, GameState::None))),
            game_gen: 0,
            location: String::new(),
            desktop: true,
            start_menu: true,
            both_cpus: false,
            installer: None,
            show_log: false,
            confirm_cancel: false,
            existing: None,
            upd: Arc::new(Mutex::new((0, UpdState::Checking))),
            upd_gen: 0,
            updating: false,
            logo: None,
        };
        match setup::find_source(&setup_exe) {
            Ok(info) => {
                app.payload_bytes = info.bytes;
                app.title = info.manifest.game_title.clone();
                app.title_ids = info.manifest.title_ids.clone();
                if let Some(l) = info.manifest.link_name.clone() {
                    app.link_name = l;
                }
                app.source = Some(info.source);
            }
            Err(e) => app.load_error = Some(e),
        }
        app.location = util::path_str(&setup::default_location(&app.title));
        app.check_reqs();
        let here = PathBuf::from(app.location.clone());
        app.find_existing(here);
        // For screenshots / testing: open on a given page.
        if let Ok(pg) = std::env::var("FABLE2_SETUP_PAGE") {
            app.page = match pg.as_str() {
                "requirements" => Page::Requirements,
                "game" => Page::Game,
                "location" => Page::Location,
                "done" => Page::Done,
                _ => Page::Welcome,
            };
            if let Ok(g) = std::env::var("FABLE2_SETUP_GAME") {
                let p = PathBuf::from(g);
                app.set_game(if p.is_dir() {
                    GameSource::Folder(p)
                } else {
                    GameSource::Iso { path: p, key: None }
                });
            }
        }
        app
    }

    /// Remember `dir` if it holds an installation and ask its installer whether GitHub has an update.
    fn find_existing(&mut self, dir: PathBuf) -> bool {
        if !setup::is_installed(&dir) {
            return false;
        }
        self.existing = Some(dir.clone());
        self.upd_gen += 1;
        let gen = self.upd_gen;
        *self.upd.lock().unwrap() = (gen, UpdState::Checking);
        let (st, repaint) = (self.upd.clone(), self.repaint.clone());
        std::thread::spawn(move || {
            let r = setup::check_update(&dir);
            let mut g = st.lock().unwrap();
            if g.0 == gen {
                g.1 = UpdState::Done(r);
            }
            drop(g);
            repaint();
        });
        true
    }

    fn start_update(&mut self) {
        let Some(dir) = self.existing.clone() else {
            return;
        };
        self.location = util::path_str(&dir);
        self.installer = Some(Installer::start_update(
            dir,
            self.title.clone(),
            self.start_menu,
            self.repaint.clone(),
        ));
        self.updating = true;
        self.confirm_cancel = false;
        self.show_log = false;
        self.page = Page::Installing;
    }

    fn update_card(&mut self, ui: &mut egui::Ui) {
        let Some(dir) = self.existing.clone() else {
            return;
        };
        let st = self.upd.lock().unwrap().1.clone();
        let mut do_update = false;
        let mut recheck = false;
        let mut other = false;
        card(ui, |ui| {
            ui.horizontal(|ui| {
                icon(
                    ui,
                    match &st {
                        UpdState::Checking => Icon::Busy,
                        UpdState::Done(Ok(i)) if i.available => Icon::Warn,
                        UpdState::Done(Ok(_)) => Icon::Ok,
                        UpdState::Done(Err(_)) => Icon::Bad,
                    },
                );
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 3.0;
                    ui.label(
                        RichText::new(format!(
                            "{} is installed in {}",
                            self.title,
                            util::path_str(&dir)
                        ))
                        .strong(),
                    );
                    let (text, color) = match &st {
                        UpdState::Checking => {
                            ("Checking GitHub for a newer version...".to_string(), MUTED)
                        }
                        UpdState::Done(Ok(i)) if i.available && i.files > 0 => (
                            format!(
                                "An update is available (version {}): {} changed file(s), {}.",
                                i.version,
                                i.files,
                                util::human_bytes(i.bytes)
                            ),
                            ACCENT_HI,
                        ),
                        UpdState::Done(Ok(i)) if i.available => (
                            "The last update did not finish; Update finishes it.".to_string(),
                            ACCENT_HI,
                        ),
                        UpdState::Done(Ok(i)) => {
                            (format!("Up to date (version {}).", i.version), OK)
                        }
                        UpdState::Done(Err(e)) => (e.clone(), BAD),
                    };
                    ui.label(RichText::new(text).small().color(color));
                });
            });
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                let can = !matches!(st, UpdState::Checking);
                if ui.add_enabled(can, primary("Update now")).on_hover_text("Download only the files that changed on GitHub, then rebuild what they affect.").clicked() {
                    do_update = true;
                }
                if ui.add_enabled(can, secondary("Check again")).clicked() {
                    recheck = true;
                }
                if ui.add(secondary("Another folder...")).clicked() {
                    other = true;
                }
            });
        });
        if do_update {
            self.start_update();
        } else if recheck {
            self.find_existing(dir);
        } else if other {
            if let Some(p) = rfd::FileDialog::new()
                .set_title("Choose the folder where the game is installed")
                .pick_folder()
            {
                if !self.find_existing(p.clone()) {
                    *self.upd.lock().unwrap() = (
                        self.upd_gen,
                        UpdState::Done(Err(format!(
                            "{} is not an installation of {}.",
                            util::path_str(&p),
                            self.title
                        ))),
                    );
                }
            }
        }
        ui.add_space(8.0);
        ui.label(
            RichText::new("To install a fresh copy instead, click Next.")
                .small()
                .color(MUTED),
        );
        ui.add_space(8.0);
    }

    fn check_reqs(&self) {
        {
            let mut r = self.reqs.lock().unwrap();
            r.python = Check::Checking;
            r.tools = Check::Checking;
        }
        let (reqs, rp) = (self.reqs.clone(), self.repaint.clone());
        std::thread::spawn(move || {
            setup::refresh_path();
            let py = setup::check_python();
            reqs.lock().unwrap().python = py;
            rp();
            let t = setup::check_build_tools();
            reqs.lock().unwrap().tools = t;
            rp();
        });
    }

    fn install_req(&self, python: bool) {
        {
            let mut r = self.reqs.lock().unwrap();
            let msg = if python {
                "Installing Python..."
            } else {
                "Installing the C++ build tools (a few GB; this can take a while)..."
            };
            if python {
                r.python = Check::Installing(msg.into());
            } else {
                r.tools = Check::Installing(msg.into());
            }
        }
        let (reqs, rp) = (self.reqs.clone(), self.repaint.clone());
        std::thread::spawn(move || {
            let log = {
                let reqs = reqs.clone();
                let rp = rp.clone();
                move |l: String| {
                    reqs.lock().unwrap().log.push(l);
                    rp();
                }
            };
            let r = if python {
                setup::install_python(&log)
            } else {
                setup::install_build_tools(&log)
            };
            let state = match r {
                Ok(()) => {
                    if python {
                        setup::check_python()
                    } else {
                        setup::check_build_tools()
                    }
                }
                Err(e) => Check::Missing(e),
            };
            let mut g = reqs.lock().unwrap();
            if python {
                g.python = state;
            } else {
                g.tools = state;
            }
            drop(g);
            rp();
        });
    }

    fn set_game(&mut self, src: GameSource) {
        self.game = Some(src.clone());
        self.game_gen += 1;
        let gen = self.game_gen;
        *self.game_state.lock().unwrap() = (gen, GameState::Checking);
        let (st, rp, ids) = (
            self.game_state.clone(),
            self.repaint.clone(),
            self.title_ids.clone(),
        );
        std::thread::spawn(move || {
            let r = match setup::check_game(&src, &ids) {
                Ok(i) => GameState::Ok(i),
                Err(p) => GameState::Problem(p),
            };
            let mut g = st.lock().unwrap();
            if g.0 == gen {
                g.1 = r;
            }
            drop(g);
            rp();
        });
    }

    fn game_info(&self) -> Option<GameInfo> {
        match &self.game_state.lock().unwrap().1 {
            GameState::Ok(i) => Some(i.clone()),
            _ => None,
        }
    }

    fn can_continue(&self) -> bool {
        match self.page {
            Page::Welcome => self.load_error.is_none(),
            Page::Requirements => {
                let r = self.reqs.lock().unwrap();
                matches!(r.python, Check::Ok(_)) && matches!(r.tools, Check::Ok(_))
            }
            Page::Game => self.game_info().is_some(),
            Page::Location => !self.location.trim().is_empty(),
            _ => false,
        }
    }

    fn start_install(&mut self) {
        let (Some(game), Some(source)) = (self.game.clone(), self.source.clone()) else {
            return;
        };
        let opts = setup::InstallOpts {
            source,
            location: PathBuf::from(self.location.trim()),
            game,
            both_cpus: self.both_cpus,
            desktop_shortcut: self.desktop,
            start_menu: self.start_menu,
            game_title: self.title.clone(),
            link_name: self.link_name.clone(),
        };
        self.installer = Some(Installer::start(opts, self.repaint.clone()));
        self.confirm_cancel = false;
        self.page = Page::Installing;
    }

    // ---- pages ---------------------------------------------------------------------------

    fn heading(ui: &mut egui::Ui, title: &str, sub: &str) {
        ui.scope(|ui| {
            ui.spacing_mut().item_spacing.y = 4.0;
            ui.label(RichText::new(title).heading().strong());
            if !sub.is_empty() {
                ui.label(RichText::new(sub).color(MUTED));
            }
        });
        ui.add_space(12.0);
    }

    fn page_welcome(&mut self, ui: &mut egui::Ui) {
        Self::heading(
            ui,
            &format!("Welcome to {} for PC", self.title),
            "This program builds the PC version of the game from your own copy of it. Albion awaits.",
        );
        if let Some(e) = &self.load_error {
            card(ui, |ui| {
                ui.horizontal(|ui| {
                    icon(ui, Icon::Bad);
                    ui.vertical(|ui| ui.label(RichText::new(e).color(BAD)));
                });
            });
            return;
        }
        if self.existing.is_some() {
            self.update_card(ui);
        }
        card(ui, |ui| {
            ui.label(RichText::new("You will need").strong());
            for (t, d) in [
                ("Your game", format!("A disc image (.iso) of {} for Xbox 360 (title {}), or the folder with its extracted files.", self.title, self.title_ids.join(", "))),
                ("Disk space", format!("About {} free.", util::human_bytes(setup::space_needed(self.payload_bytes, 7 << 30)))),
                ("Your PC", "Windows 10 or 11, 64-bit, and a DirectX 12 graphics card. Any 64-bit processor from about 2009 on works; with AVX2 (most from 2013 on) you get the faster build.".into()),
                ("Time", "The first build takes roughly 30 to 90 minutes, depending on your PC. It runs on its own.".into()),
            ] {
                ui.horizontal(|ui| {
                    icon(ui, Icon::Dot);
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        ui.label(RichText::new(t).strong());
                        ui.label(RichText::new(d).color(MUTED));
                    });
                });
            }
        });
        ui.add_space(6.0);
        ui.label(RichText::new("No game files are included in this program. Everything is made from your own disc on your PC.").small().color(MUTED));
    }

    fn page_requirements(&mut self, ui: &mut egui::Ui) {
        Self::heading(
            ui,
            "Requirements",
            "The game is compiled on your PC, so it needs these free tools.",
        );
        let (python, tools, nlog) = {
            let r = self.reqs.lock().unwrap();
            (r.python.clone(), r.tools.clone(), r.log.len())
        };
        let busy = matches!(python, Check::Installing(_)) || matches!(tools, Check::Installing(_));
        let mut install = None;
        for (i, (name, what, c)) in [
            (
                "Python 3",
                "Runs the tools that unpack the game's program and apply the port's changes.",
                &python,
            ),
            (
                "Visual Studio Build Tools + Clang",
                "The C++ compiler. A free download from Microsoft (several GB).",
                &tools,
            ),
        ]
        .into_iter()
        .enumerate()
        {
            card(ui, |ui| {
                ui.horizontal(|ui| {
                    icon(
                        ui,
                        match c {
                            Check::Ok(_) => Icon::Ok,
                            Check::Missing(_) => Icon::Bad,
                            _ => Icon::Busy,
                        },
                    );
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 3.0;
                        ui.horizontal(|ui| {
                            ui.label(RichText::new(name).strong());
                            if let Check::Missing(_) = c {
                                ui.with_layout(
                                    egui::Layout::right_to_left(egui::Align::Center),
                                    |ui| {
                                        if ui.add_enabled(!busy, primary("Install")).clicked() {
                                            install = Some(i == 0);
                                        }
                                    },
                                );
                            }
                        });
                        let (txt, col) = match c {
                            Check::Ok(s) => (s.clone(), MUTED),
                            Check::Missing(s) => (format!("Not found. {}\n{s}", what), WARN),
                            Check::Installing(s) => (s.clone(), ACCENT_HI),
                            Check::Checking | Check::Unknown => ("Checking...".into(), MUTED),
                        };
                        ui.label(RichText::new(txt).small().color(col));
                    });
                });
            });
        }
        if let Some(py) = install {
            self.install_req(py);
        }
        ui.horizontal(|ui| {
            if ui.add_enabled(!busy, secondary("Check again")).clicked() {
                self.check_reqs();
            }
            if nlog > 0
                && ui
                    .add(secondary(if self.show_req_log {
                        "Hide details"
                    } else {
                        "Show details"
                    }))
                    .clicked()
            {
                self.show_req_log = !self.show_req_log;
            }
        });
        if self.show_req_log {
            let log = self.reqs.lock().unwrap().log.join("\n");
            log_box(ui, &log, 150.0);
        }
    }

    fn page_game(&mut self, ui: &mut egui::Ui) {
        Self::heading(
            ui,
            "Your game",
            &format!(
                "Choose your {} disc image, or a folder with the game's files.",
                self.title
            ),
        );
        ui.horizontal(|ui| {
            if ui.add(primary("Choose disc image (.iso)...")).clicked() {
                if let Some(p) = rfd::FileDialog::new()
                    .set_title("Choose your Fable II disc image")
                    .add_filter("Xbox 360 disc image", &["iso", "ISO"])
                    .pick_file()
                {
                    self.set_game(GameSource::Iso { path: p, key: None });
                }
            }
            if ui.add(secondary("Use a game folder...")).clicked() {
                if let Some(p) = rfd::FileDialog::new()
                    .set_title("Choose the folder that contains default.xex")
                    .pick_folder()
                {
                    self.set_game(GameSource::Folder(p));
                }
            }
        });
        ui.add_space(4.0);
        let state = self.game_state.lock().unwrap().1.clone();
        let Some(src) = self.game.clone() else {
            ui.label(RichText::new("Full (Redump) disc images and trimmed XISO images both work. The game disc's own files are needed: a title update or a Games on Demand copy has a different program file.").small().color(MUTED));
            return;
        };
        let shown = match &src {
            GameSource::Iso { path, .. } => path.clone(),
            GameSource::Folder(p) => p.clone(),
        };
        card(ui, |ui| {
            ui.horizontal(|ui| {
                icon(
                    ui,
                    match &state {
                        GameState::Ok(_) => Icon::Ok,
                        GameState::Checking | GameState::None => Icon::Busy,
                        GameState::Problem(_) => Icon::Bad,
                    },
                );
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 3.0;
                    match &state {
                        GameState::Ok(i) => {
                            ui.label(
                                RichText::new(format!("{}  ·  {}", i.title, i.title_id)).strong(),
                            );
                        }
                        GameState::Checking | GameState::None => {
                            ui.label(RichText::new("Checking...").strong());
                        }
                        GameState::Problem(p) => {
                            ui.label(RichText::new(p.to_string()).strong().color(BAD));
                        }
                    }
                    let mut sub = util::path_str(&shown);
                    if let GameState::Ok(i) = &state {
                        sub.push_str(&format!("\nmedia {}  ·  version {}", i.media_id, i.version));
                        if i.copy_bytes > 0 {
                            sub.push_str(&format!(
                                "  ·  {} of game files",
                                util::human_bytes(i.copy_bytes)
                            ));
                        }
                    }
                    ui.label(RichText::new(sub).small().color(MUTED));
                });
            });
        });
    }

    fn page_location(&mut self, ui: &mut egui::Ui) {
        Self::heading(
            ui,
            "Install location",
            "Where should the game be built? Pick a drive with plenty of space.",
        );
        card(ui, |ui| {
            ui.label(RichText::new("Folder").strong());
            ui.horizontal(|ui| {
                ui.add(
                    egui::TextEdit::singleline(&mut self.location)
                        .desired_width(ui.available_width() - 120.0),
                );
                if ui.add(secondary("Browse...")).clicked() {
                    let mut d = rfd::FileDialog::new().set_title("Choose where to install");
                    if let Some(p) = Path::new(self.location.trim())
                        .parent()
                        .filter(|p| p.is_dir())
                    {
                        d = d.set_directory(p);
                    }
                    if let Some(p) = d.pick_folder() {
                        // An empty folder or an earlier install is used as is; otherwise a subfolder.
                        let empty = std::fs::read_dir(&p)
                            .map(|mut r| r.next().is_none())
                            .unwrap_or(true);
                        let ours = p.join("installer").join("fable2-installer.toml").is_file();
                        self.location = util::path_str(&if empty || ours {
                            p
                        } else {
                            p.join(format!("{} PC", self.title))
                        });
                    }
                }
            });
            let game_bytes = self.game_info().map(|i| i.copy_bytes).unwrap_or(0);
            let need = setup::space_needed(self.payload_bytes, game_bytes);
            let free = util::free_space(Path::new(self.location.trim()));
            let (txt, col) = match free {
                Some(f) if f < need => (
                    format!(
                        "Needs about {}, but only {} is free on that drive.",
                        util::human_bytes(need),
                        util::human_bytes(f)
                    ),
                    BAD,
                ),
                Some(f) => (
                    format!(
                        "Needs about {}  ·  {} free",
                        util::human_bytes(need),
                        util::human_bytes(f)
                    ),
                    MUTED,
                ),
                None => (format!("Needs about {}", util::human_bytes(need)), MUTED),
            };
            ui.label(RichText::new(txt).small().color(col));
            if self.location.contains(['(', ')', '&', '%', '!', '^']) {
                ui.label(
                    RichText::new(
                        "Tip: a path without ( ) & % ! ^ avoids trouble with the build tools.",
                    )
                    .small()
                    .color(WARN),
                );
            }
        });
        card(ui, |ui| {
            ui.checkbox(&mut self.desktop, "Create a desktop shortcut");
            ui.checkbox(&mut self.start_menu, "Add to the Start menu");
        });
        card(ui, |ui| {
            let fast = util::cpu_has_avx2();
            ui.label(RichText::new("Processor").strong());
            ui.label(
                RichText::new(if fast {
                    "Your processor has AVX2, so the fast version of the game is built."
                } else if util::cpu_has_compat_floor() {
                    "Your processor has no AVX2, so the compatible version is built. It runs the same game, a little slower."
                } else {
                    "Your processor lacks SSE4.2, which the game needs. The build will finish, but the game will not start on this PC."
                })
                .small()
                .color(if fast || util::cpu_has_compat_floor() { MUTED } else { WARN }),
            );
            ui.checkbox(
                &mut self.both_cpus,
                if fast {
                    "Also build the version for processors without AVX2 (to copy this folder to an older PC)"
                } else {
                    "Also build the fast AVX2 version (to copy this folder to a newer PC)"
                },
            );
            if self.both_cpus {
                ui.label(
                    RichText::new("This roughly doubles the build time and needs about 4 GB more space. The launcher picks the right version each time it starts.")
                        .small()
                        .color(MUTED),
                );
            }
        });
        ui.label(RichText::new("When you click Install, the game is copied from your disc and compiled. It runs by itself; you can leave it alone.").small().color(MUTED));
    }

    fn page_installing(&mut self, ui: &mut egui::Ui) {
        let Some(inst) = &self.installer else { return };
        let (fraction, phase, detail, done, log, secs) = {
            let p = inst.progress.lock().unwrap();
            (
                p.fraction,
                p.phase.clone(),
                p.detail.clone(),
                p.done.clone(),
                if self.show_log {
                    p.log.join("\n")
                } else {
                    String::new()
                },
                p.started.map(|s| s.elapsed().as_secs()).unwrap_or(0),
            )
        };
        match &done {
            Some(Ok(())) => {
                self.page = Page::Done;
                return;
            }
            Some(Err(e)) => {
                Self::heading(
                    ui,
                    "Something went wrong",
                    if self.updating {
                        "The update stopped. Nothing is lost: run the update again and it continues where it stopped."
                    } else {
                        "The installation stopped. Nothing is lost: Try again continues where it stopped."
                    },
                );
                card(ui, |ui| {
                    ui.horizontal(|ui| {
                        icon(ui, Icon::Bad);
                        ui.vertical(|ui| ui.label(RichText::new(e).color(BAD)));
                    });
                });
            }
            None => {
                Self::heading(
                    ui,
                    if self.updating {
                        "Updating"
                    } else {
                        "Installing"
                    },
                    &format!("Elapsed {}:{:02}", secs / 60, secs % 60),
                );
                card(ui, |ui| {
                    ui.label(
                        RichText::new(if phase.is_empty() {
                            "Starting..."
                        } else {
                            &phase
                        })
                        .strong()
                        .size(16.5),
                    );
                    ui.add(
                        egui::ProgressBar::new(fraction)
                            .fill(ACCENT)
                            .show_percentage()
                            .animate(true),
                    );
                    ui.label(
                        RichText::new(if detail.is_empty() { " " } else { &detail })
                            .small()
                            .color(MUTED),
                    );
                });
            }
        }
        ui.horizontal(|ui| {
            if ui
                .add(secondary(if self.show_log {
                    "Hide details"
                } else {
                    "Show details"
                }))
                .clicked()
            {
                self.show_log = !self.show_log;
            }
            if done.is_some() {
                if ui.add(secondary("Copy details")).clicked() {
                    let all = inst.progress.lock().unwrap().log.join("\n");
                    ui.output_mut(|o| o.copied_text = all);
                }
                if ui.add(secondary("Open folder")).clicked() {
                    setup::open_folder(Path::new(self.location.trim()));
                }
            }
        });
        if self.show_log {
            log_box(ui, &log, 230.0);
        }
        if done.is_none() {
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(500));
        }
    }

    fn page_done(&mut self, ui: &mut egui::Ui) {
        if self.updating {
            Self::heading(
                ui,
                &format!("{} is up to date", self.title),
                "The latest version from GitHub is installed.",
            );
        } else {
            Self::heading(
                ui,
                &format!("{} is ready", self.title),
                "The game was built successfully. Your adventure in Albion can begin.",
            );
        }
        card(ui, |ui| {
            ui.horizontal(|ui| {
                icon(ui, Icon::Ok);
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 3.0;
                    ui.label(
                        RichText::new(format!("Installed to {}", self.location.trim())).strong(),
                    );
                    let s = match (self.desktop, self.start_menu) {
                        (true, true) => {
                            "Start it any time from the desktop shortcut or the Start menu."
                                .to_string()
                        }
                        (true, false) => "Start it any time from the desktop shortcut.".to_string(),
                        (false, true) => "Start it any time from the Start menu.".to_string(),
                        (false, false) => {
                            format!("Start it any time with {}.exe in that folder.", self.title)
                        }
                    };
                    ui.label(RichText::new(s).small().color(MUTED));
                });
            });
        });
        ui.add_space(10.0);
        ui.horizontal(|ui| {
            if ui
                .add(primary(&format!("Play {}", self.title)).min_size(egui::vec2(200.0, 46.0)))
                .clicked()
            {
                let exe = Path::new(self.location.trim()).join(format!("{}.exe", self.title));
                let _ = std::process::Command::new(exe)
                    .current_dir(self.location.trim())
                    .spawn();
                ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close);
            }
            if ui.add(secondary("Open install folder")).clicked() {
                setup::open_folder(Path::new(self.location.trim()));
            }
        });
        ui.add_space(6.0);
        ui.label(RichText::new(format!("To update later, use \"Update {}\" in the Start menu (or run this setup again): it downloads only what changed.", self.title)).small().color(MUTED));
    }
}

fn log_box(ui: &mut egui::Ui, text: &str, h: f32) {
    egui::Frame::none()
        .fill(Color32::from_rgb(0x07, 0x0A, 0x14))
        .stroke(Stroke::new(1.0_f32, CARD_EDGE))
        .rounding(8.0_f32)
        .inner_margin(10.0_f32)
        .show(ui, |ui| {
            egui::ScrollArea::vertical()
                .max_height(h)
                .stick_to_bottom(true)
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    ui.label(
                        RichText::new(text)
                            .monospace()
                            .color(Color32::from_rgb(0xCB, 0xC4, 0xB2)),
                    );
                });
        });
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // ---- sidebar ----
        egui::SidePanel::left("side")
            .exact_width(232.0)
            .resizable(false)
            .frame(egui::Frame::none().fill(SIDE).inner_margin(egui::Margin {
                left: 26.0,
                right: 18.0,
                top: 30.0,
                bottom: 20.0,
            }))
            .show(ctx, |ui| {
                let logo = self
                    .logo
                    .get_or_insert_with(|| {
                        let n = 128;
                        ctx.load_texture(
                            "logo",
                            egui::ColorImage::from_rgba_unmultiplied(
                                [n, n],
                                &fable2_installer::brand::icon_rgba(n),
                            ),
                            egui::TextureOptions::LINEAR,
                        )
                    })
                    .id();
                ui.add(
                    egui::Image::new(egui::load::SizedTexture::new(logo, egui::vec2(56.0, 56.0)))
                        .fit_to_exact_size(egui::vec2(56.0, 56.0)),
                );
                ui.add_space(8.0);
                ui.scope(|ui| {
                    ui.spacing_mut().item_spacing.y = 2.0;
                    ui.label(
                        RichText::new(&self.title)
                            .size(24.0)
                            .family(serif(true))
                            .color(TEXT),
                    );
                    ui.label(
                        RichText::new("ALBION  \u{00B7}  PC EDITION")
                            .size(11.5)
                            .color(GOLD)
                            .strong(),
                    );
                });
                ui.add_space(14.0);
                // a little gold flourish: rule, diamond, rule
                let (rule, _) =
                    ui.allocate_exact_size(egui::vec2(96.0, 9.0), egui::Sense::hover());
                let pt = ui.painter();
                let c = rule.left_center() + egui::vec2(48.0, 0.0);
                let g = GOLD.linear_multiply(0.8);
                pt.line_segment([rule.left_center(), c - egui::vec2(7.0, 0.0)], Stroke::new(1.0_f32, g));
                pt.line_segment([c + egui::vec2(7.0, 0.0), rule.right_center()], Stroke::new(1.0_f32, g));
                pt.add(egui::Shape::convex_polygon(
                    vec![
                        c + egui::vec2(0.0, -4.0),
                        c + egui::vec2(4.0, 0.0),
                        c + egui::vec2(0.0, 4.0),
                        c + egui::vec2(-4.0, 0.0),
                    ],
                    g,
                    Stroke::NONE,
                ));
                ui.add_space(20.0);
                for (p, name) in PAGES {
                    ui.horizontal(|ui| {
                        let (r, _) =
                            ui.allocate_exact_size(egui::vec2(18.0, 18.0), egui::Sense::hover());
                        let c = r.center();
                        let pt = ui.painter();
                        if p < self.page {
                            pt.circle_filled(c, 8.0, ACCENT.linear_multiply(0.35));
                            let s = Stroke::new(2.0_f32, ACCENT_HI);
                            pt.line_segment(
                                [c + egui::vec2(-3.5, 0.3), c + egui::vec2(-1.0, 2.8)],
                                s,
                            );
                            pt.line_segment(
                                [c + egui::vec2(-1.0, 2.8), c + egui::vec2(3.8, -2.6)],
                                s,
                            );
                        } else if p == self.page {
                            pt.circle_filled(c, 8.0, ACCENT);
                            pt.circle_filled(c, 3.0, SIDE);
                        } else {
                            pt.circle_stroke(
                                c,
                                7.0,
                                Stroke::new(1.5_f32, Color32::from_rgb(0x3A, 0x43, 0x66)),
                            );
                        }
                        let t = RichText::new(name).size(15.0);
                        ui.label(if p == self.page {
                            t.strong().color(TEXT)
                        } else if p < self.page {
                            t.color(Color32::from_rgb(0xC9, 0xBF, 0xA8))
                        } else {
                            t.color(MUTED)
                        });
                    });
                    ui.add_space(2.0);
                }
                ui.with_layout(egui::Layout::bottom_up(egui::Align::Min), |ui| {
                    ui.label(
                        RichText::new(
                            "Fan-made PC port. Not affiliated with Microsoft, Xbox Game Studios or Lionhead Studios.",
                        )
                        .size(11.0)
                        .color(Color32::from_rgb(0x5E, 0x66, 0x82)),
                    );
                });
            });

        // ---- navigation bar ----
        egui::TopBottomPanel::bottom("nav")
            .exact_height(70.0)
            .frame(
                egui::Frame::none()
                    .fill(BG)
                    .inner_margin(egui::Margin {
                        left: 30.0,
                        right: 30.0,
                        top: 14.0,
                        bottom: 16.0,
                    })
                    .stroke(Stroke::new(1.0_f32, Color32::from_rgb(0x1F, 0x25, 0x3D))),
            )
            .show(ctx, |ui| {
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    match self.page {
                        Page::Welcome | Page::Requirements | Page::Game => {
                            if ui
                                .add_enabled(self.can_continue(), primary("Next"))
                                .clicked()
                            {
                                self.page = match self.page {
                                    Page::Welcome => Page::Requirements,
                                    Page::Requirements => Page::Game,
                                    _ => Page::Location,
                                };
                            }
                        }
                        Page::Location => {
                            if ui
                                .add_enabled(self.can_continue(), primary("Install"))
                                .clicked()
                            {
                                self.start_install();
                            }
                        }
                        Page::Installing => {
                            let done = self
                                .installer
                                .as_ref()
                                .and_then(|i| i.progress.lock().unwrap().done.clone());
                            match done {
                                Some(Err(_)) => {
                                    if ui.add(primary("Try again")).clicked() {
                                        if self.updating {
                                            self.start_update();
                                        } else {
                                            self.start_install();
                                        }
                                    }
                                    if ui.add(secondary("Back")).clicked() {
                                        if self.updating {
                                            self.updating = false;
                                            self.page = Page::Welcome;
                                            if let Some(d) = self.existing.clone() {
                                                self.find_existing(d);
                                            }
                                        } else {
                                            self.page = Page::Location;
                                        }
                                    }
                                }
                                _ => {
                                    let label = if self.confirm_cancel {
                                        "Click again to stop"
                                    } else {
                                        "Cancel"
                                    };
                                    if ui.add(secondary(label)).clicked() {
                                        if self.confirm_cancel {
                                            if let Some(i) = &self.installer {
                                                i.stop();
                                            }
                                        } else {
                                            self.confirm_cancel = true;
                                        }
                                    }
                                }
                            }
                        }
                        Page::Done => {
                            if ui.add(secondary("Close")).clicked() {
                                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                            }
                        }
                    }
                    if matches!(self.page, Page::Requirements | Page::Game | Page::Location)
                        && ui.add(secondary("Back")).clicked()
                    {
                        self.page = match self.page {
                            Page::Requirements => Page::Welcome,
                            Page::Game => Page::Requirements,
                            _ => Page::Game,
                        };
                    }
                });
            });

        // ---- page ----
        egui::CentralPanel::default()
            .frame(egui::Frame::none().fill(BG).inner_margin(egui::Margin {
                left: 34.0,
                right: 34.0,
                top: 30.0,
                bottom: 10.0,
            }))
            .show(ctx, |ui| {
                egui::ScrollArea::vertical()
                    .auto_shrink([false, false])
                    .show(ui, |ui| match self.page {
                        Page::Welcome => self.page_welcome(ui),
                        Page::Requirements => self.page_requirements(ui),
                        Page::Game => self.page_game(ui),
                        Page::Location => self.page_location(ui),
                        Page::Installing => self.page_installing(ui),
                        Page::Done => self.page_done(ui),
                    });
            });

        // Keep spinners moving while something runs in the background.
        let busy = {
            let r = self.reqs.lock().unwrap();
            matches!(r.python, Check::Checking | Check::Installing(_))
                || matches!(r.tools, Check::Checking | Check::Installing(_))
        } || matches!(self.game_state.lock().unwrap().1, GameState::Checking);
        if busy {
            ctx.request_repaint_after(std::time::Duration::from_millis(60));
        }
    }
}

impl Drop for App {
    fn drop(&mut self) {
        if let Some(i) = &self.installer {
            if i.progress.lock().unwrap().done.is_none() {
                i.stop();
            }
        }
    }
}

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([900.0, 600.0])
            .with_min_inner_size([820.0, 560.0])
            .with_title("Fable II Setup")
            .with_icon(window_icon()),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "Fable II Setup",
        options,
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )
}
