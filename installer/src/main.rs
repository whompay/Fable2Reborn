//! fable2-installer: builds the Fable II (Xbox 360, title 4D5307F1) PC recompilation from the
//! player's own copy of the game. No game code or data ships with the port; this tool regenerates it.
//!
//!   fable2-installer                       interactive install (double-click)
//!   fable2-installer install --game <dir>  regenerate, patch, build, create the launcher
//!   fable2-installer doctor                check Python / Visual Studio / clang / CMake / Ninja
//!   fable2-installer update                download what changed on GitHub, rebuild what that needs
//!   fable2-installer make-patches --game <dir>   (maintainer) rebuild installer/patches + lock
//!   fable2-installer export --out <dir>    (maintainer) copy the publishable source tree

use fable2_installer::{
    bail, game, hash, iso, lock, manifest, payload, pipeline, toolchain, update, util,
};
use manifest::Manifest;
use pipeline::Vars;
use std::path::{Path, PathBuf};
use std::process::Command;
use util::{Ctx, Result};

const USAGE: &str = "\
fable2-installer -- build the Fable II PC port from your own game files

USAGE
  fable2-installer [install] [options]     regenerate the code from your game, build, make a launcher
  fable2-installer doctor  [options]       check that every tool the build needs is installed
  fable2-installer update  [options]       get the latest version from GitHub (only the files that
                                          changed), then rebuild what that needs
  fable2-installer make-patches --game DIR (maintainers) rebuild installer/patches from your tree
  fable2-installer export --out DIR        (maintainers) copy the publishable source tree to DIR
  fable2-installer package --out FILE.exe  (maintainers) build the one-file setup program
  fable2-installer extract-iso --iso FILE --out DIR
                                          copy an Xbox 360 disc image's files (Redump or XISO)

OPTIONS
  --game DIR        your extracted disc: the folder that contains default.xex and data
  --iso FILE        your disc image; its files are copied into the checkout's extracted/ folder
  --python EXE      Python 3.10+ to use (default: py launcher / python on PATH)
  --root DIR        the port checkout (default: found from the installer's location)
  --jobs N          worker processes for the lifters (default: CPU count)
  --build-jobs N    parallel compiler jobs (default: 4; each needs several GB of RAM)
  --regen           redo every generation step instead of resuming
  --no-build        stop after generating and patching the sources
  --cpu WHICH       auto (default): the fast AVX2 build if this PC's processor has AVX2, else the
                    compatible one (plus any build an earlier install made); avx2, compat, or both
  --force           continue past the game-version and existing-files checks
  --allow-leaks     (make-patches) write the patch set even if it would carry generated content
  --keep-fresh      (make-patches) reuse .installer/fresh from the last run
  -y, --yes         don't ask questions; fail instead
  --check           (update) only report whether an update is available
  --repo OWNER/NAME (update) repository to update from (default: [update] in fable2-installer.toml)
  --branch NAME     (update) branch to update from (default: [update] branch, else main)
";

#[derive(Default)]
struct Opts {
    cmd: String,
    game: Option<PathBuf>,
    python: Option<PathBuf>,
    root: Option<PathBuf>,
    out: Option<PathBuf>,
    iso: Option<PathBuf>,
    jobs: Option<usize>,
    build_jobs: Option<usize>,
    regen: bool,
    no_build: bool,
    force: bool,
    allow_leaks: bool,
    keep_fresh: bool,
    yes: bool,
    interactive: bool,
    check: bool,
    repo: Option<String>,
    branch: Option<String>,
    /// (install, set by update) keep the timestamps of generated files whose content did not
    /// change, so regenerating does not force the compiler to rebuild everything.
    keep_mtimes: bool,
    /// --cpu auto | avx2 | compat | both
    cpu: Option<String>,
}

fn parse_args() -> Result<Opts> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    // Double-click (no args) or a folder dropped onto the exe: keep the window open at the end.
    let dropped = args.len() == 1 && !args[0].starts_with('-') && Path::new(&args[0]).exists();
    let mut o = Opts {
        interactive: args.is_empty() || dropped,
        ..Default::default()
    };
    let mut i = 0;
    let val = |i: &mut usize, name: &str| -> Result<String> {
        *i += 1;
        args.get(*i)
            .cloned()
            .ctx(|| format!("{name} needs a value"))
    };
    while i < args.len() {
        let a = args[i].clone();
        match a.as_str() {
            "install" | "doctor" | "update" | "make-patches" | "export" | "package"
            | "extract-iso" | "help"
                if o.cmd.is_empty() =>
            {
                o.cmd = a
            }
            "-h" | "--help" => o.cmd = "help".into(),
            "--game" => o.game = Some(val(&mut i, &a)?.into()),
            "--python" => o.python = Some(val(&mut i, &a)?.into()),
            "--root" => o.root = Some(val(&mut i, &a)?.into()),
            "--out" => o.out = Some(val(&mut i, &a)?.into()),
            "--iso" => o.iso = Some(val(&mut i, &a)?.into()),
            "--jobs" => o.jobs = Some(val(&mut i, &a)?.parse().ctx(|| "--jobs needs a number")?),
            "--build-jobs" => {
                o.build_jobs = Some(
                    val(&mut i, &a)?
                        .parse()
                        .ctx(|| "--build-jobs needs a number")?,
                )
            }
            "--regen" => o.regen = true,
            "--no-build" => o.no_build = true,
            "--force" => o.force = true,
            "--cpu" => {
                let v = val(&mut i, &a)?.to_ascii_lowercase();
                if !["auto", "avx2", "compat", "both"].contains(&v.as_str()) {
                    bail!("--cpu takes auto, avx2, compat or both");
                }
                o.cpu = Some(v)
            }
            "--allow-leaks" => o.allow_leaks = true,
            "--keep-fresh" => o.keep_fresh = true,
            "-y" | "--yes" => o.yes = true,
            "--check" => o.check = true,
            "--repo" => o.repo = Some(val(&mut i, &a)?),
            "--branch" => o.branch = Some(val(&mut i, &a)?),
            // A folder or a disc image dropped onto the exe.
            _ if !a.starts_with('-')
                && o.game.is_none()
                && o.iso.is_none()
                && Path::new(&a).exists() =>
            {
                if Path::new(&a).is_file() && !a.to_ascii_lowercase().ends_with(".xex") {
                    o.iso = Some(a.into())
                } else {
                    o.game = Some(a.into())
                }
            }
            _ => bail!("unknown argument '{a}' (try --help)"),
        }
        i += 1;
    }
    if o.cmd.is_empty() {
        o.cmd = "install".into();
    }
    Ok(o)
}

/// The checkout root is the folder holding `installer/fable2-installer.toml`.
fn find_root(explicit: Option<&Path>) -> Result<PathBuf> {
    let has = |d: &Path| d.join("installer").join("fable2-installer.toml").is_file();
    if let Some(r) = explicit {
        if has(r) {
            return Ok(util::absolute(r));
        }
        bail!("{} has no installer/fable2-installer.toml", r.display());
    }
    // The working directory first (running from inside a checkout), then the exe's location
    // (double-clicked from <root>\ or <root>\installer\).
    let mut starts = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        starts.push(cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        starts.push(util::absolute(&exe));
    }
    for s in starts {
        let mut d: Option<&Path> = Some(&s);
        for _ in 0..8 {
            let Some(x) = d else { break };
            if has(x) {
                return Ok(util::absolute(x));
            }
            d = x.parent();
        }
    }
    bail!("could not find the port checkout (a folder with installer/fable2-installer.toml). Run the installer from inside it, or pass --root.")
}

struct Session {
    o: Opts,
    root: PathBuf,
    installer_dir: PathBuf,
    m: Manifest,
}

impl Session {
    fn jobs(&self) -> usize {
        self.o.jobs.unwrap_or_else(|| {
            std::thread::available_parallelism()
                .map(|n| n.get())
                .unwrap_or(4)
        })
    }

    /// The game folder: --game, --iso (copied into the checkout), the existing folder or link in
    /// the checkout, or ask.
    fn game(&self) -> Result<game::Game> {
        if let Some(g) = &self.o.game {
            return game::locate(g);
        }
        if let Some(i) = &self.o.iso {
            return self.game_from_iso(i);
        }
        if let Some(l) = &self.m.link_name {
            let p = self.root.join(l);
            if p.join("default.xex").is_file() {
                return game::locate(&p);
            }
        }
        if self.o.yes {
            bail!("pass --game <folder that contains default.xex> or --iso <disc image>");
        }
        println!();
        println!(
            "Where is your {} game? (the disc image .iso, or the folder that contains default.xex; you can drag it here)",
            self.m.game_title
        );
        loop {
            let a = util::prompt("> ");
            let a = a.trim().trim_matches('"').to_string();
            if a.is_empty() {
                bail!("no game given");
            }
            let p = Path::new(&a);
            let r = if p.is_file() && !a.to_ascii_lowercase().ends_with(".xex") {
                self.game_from_iso(p)
            } else {
                game::locate(p)
            };
            match r {
                Ok(g) => return Ok(g),
                Err(e) => println!("    {e}"),
            }
        }
    }

    /// Copy the disc image's files into `<root>/<link_name>` (resumable), then use them.
    fn game_from_iso(&self, iso_path: &Path) -> Result<game::Game> {
        let name = self
            .m
            .link_name
            .clone()
            .unwrap_or_else(|| "extracted".into());
        let dest = self.root.join(&name);
        if std::fs::symlink_metadata(&dest)
            .map(|m| m.file_type().is_symlink())
            .unwrap_or(false)
        {
            bail!(
                "{} is a link to another folder; remove it (or pass --game) before copying a disc image here",
                dest.display()
            );
        }
        let meta =
            std::fs::metadata(iso_path).ctx(|| format!("reading {}", iso_path.display()))?;
        let stamp = format!("{}|{}", iso_path.display(), meta.len());
        let marker = dest.join(".copied-from-disc-image");
        if std::fs::read_to_string(&marker)
            .map(|s| s == stamp)
            .unwrap_or(false)
        {
            util::info("The game files were already copied from this disc image.");
        } else {
            util::heading("Copying the game from your disc image");
            extract_iso_to(iso_path, &dest)?;
            util::write_file(&marker, stamp.as_bytes())?;
        }
        game::locate(&dest)
    }

    fn check_title(&self, g: &game::Game) -> Result<()> {
        util::info(&format!(
            "Game: {} [{}], media {}, version {} at {}",
            g.title(),
            g.title_id(),
            g.info.media_id,
            g.info.version,
            g.root.display()
        ));
        if !self.m.title_ids.is_empty() && !self.m.title_ids.iter().any(|t| t == g.title_id()) {
            let msg = format!(
                "this port is for {} ({}), but the dump is {} [{}]",
                self.m.game_title,
                self.m.title_ids.join(", "),
                g.title(),
                g.title_id()
            );
            if !self.o.force {
                bail!("{msg}. (--force tries anyway; it will almost certainly fail.)");
            }
            util::warn(&msg);
        }
        Ok(())
    }

    fn python(&self) -> Result<PathBuf> {
        util::heading("Python");
        let (base, v) = toolchain::find_python(self.o.python.as_deref(), self.m.py_min)?;
        util::info(&format!(
            "Using Python {}.{} ({})",
            v.0,
            v.1,
            base.prog.join(" ")
        ));
        let reqs: Vec<PathBuf> = self
            .m
            .py_requirements
            .iter()
            .map(|r| util::join_slash(&self.root, r))
            .collect();
        for r in &reqs {
            if !r.is_file() {
                bail!("missing {}", r.display());
            }
        }
        toolchain::prepare_venv(
            &base,
            &self.root.join(".installer").join("venv"),
            &reqs,
            &self.m.py_packages,
        )
    }
}

fn xex_hash_check(s: &Session, xex: &Path, lock: Option<&lock::Lock>) -> Result<String> {
    let sha = hash::sha256_file(xex)?;
    util::info(&format!("default.xex sha256 {sha}"));
    let mut expected: Vec<String> = s.m.xex_sha256.clone();
    if let Some(e) = lock.and_then(|l| l.xex.clone()) {
        expected.push(e);
    }
    if !expected.is_empty() && !expected.contains(&sha) {
        let msg = "this default.xex is not the version the port was made from (expected Fable II, title 4D5307F1, media 716F0A0D: the executable on the game disc)";
        if !s.o.force {
            bail!("{msg}.\n    Copy default.xex from the disc again (title updates replace it). --force tries anyway; expect the patch step to fail.");
        }
        util::warn(msg);
    }
    Ok(sha)
}

fn cmd_install(s: &Session) -> Result<()> {
    println!("{} PC port installer", s.m.game_title);
    println!("Checkout: {}", s.root.display());
    let lock = lock::Lock::load(&s.installer_dir.join("generated.lock"))?;
    if lock.is_none() {
        bail!(
            "installer/generated.lock is missing, so there is no patch set for this checkout.\n    \
             (Maintainers: run `fable2-installer make-patches --game <dir>` on the development tree.)"
        );
    }
    let lock = lock.unwrap();

    util::heading("Game files");
    let g = s.game()?;
    s.check_title(&g)?;

    // A tree whose generated files are already final (the maintainer's own tree, or an earlier
    // install that finished) only needs building.
    util::heading("Checking the generated sources");
    let ready = !s.o.regen && pipeline::lock_satisfied(&s.root, &lock)?;
    if ready {
        util::info(&format!(
            "All {} generated files already match the port's patch set.",
            lock.files.len()
        ));
    }

    // Refuse to overwrite a development tree's hand-maintained generated files.
    let state = lock::State::load(&s.root.join(".installer").join("state"));
    if !ready && state.xex.is_none() && !s.o.force {
        let existing = util::select_files(
            &s.root,
            &s.m.gen_include,
            &s.m.gen_exclude,
            pipeline::SKIP_DIRS,
        )?;
        let existing: Vec<String> = existing.into_iter().filter(|f| !s.m.is_loose(f)).collect();
        if !existing.is_empty() {
            bail!(
                "{} generated file(s) already exist here (e.g. {}) and do not match the port's patch set.\n    \
                 This looks like a development tree with unpublished edits; installing would overwrite them.\n    \
                 Run `make-patches` first, use a clean checkout (or `export`), or pass --force to replace them.",
                existing.len(),
                existing[0]
            );
        }
    }

    let py = s.python()?;
    util::heading("Executable");
    let sha = xex_hash_check(s, &g.xex, Some(&lock))?;

    if let Some(l) = &s.m.link_name {
        pipeline::link_game(&s.root, l, &g.root)?;
    }

    let mut vars = pipeline::workspace_vars(&s.m, &s.root, &s.installer_dir);
    pipeline::game_vars(&mut vars, &g);
    vars.set_path("python", &py);
    vars.set("jobs", s.jobs().to_string());

    if ready {
        util::info("Skipping regeneration (--regen redoes it).");
    } else {
        let before = if s.o.keep_mtimes {
            snapshot_generated(s, &lock)?
        } else {
            Vec::new()
        };
        util::heading("Regenerating the game code from your files");
        let failed = pipeline::run_steps(&s.m, &s.root, &vars, &sha, s.o.regen)?;
        if !failed.is_empty() {
            util::warn(&format!(
                "optional steps failed: {} (the matching features stay off)",
                failed.join(", ")
            ));
        }

        util::heading("Applying the port's patches");
        let tolerate: Vec<String> =
            s.m.steps
                .iter()
                .filter(|st| failed.contains(&st.id))
                .flat_map(|st| st.outputs.clone())
                .collect();
        pipeline::apply_lock(&s.m, &s.root, &s.installer_dir, &lock, &tolerate)?;
        let state_path = s.root.join(".installer").join("state");
        let mut st = lock::State::load(&state_path);
        st.applied = true;
        st.save(&state_path)?;
        if !before.is_empty() {
            let kept = restore_mtimes(&before);
            util::info(&format!("{kept} of {} generated file(s) unchanged by this update (they will not be recompiled)", before.len()));
        }
    }

    if s.o.no_build {
        util::heading("Sources are ready (--no-build)");
        return Ok(());
    }
    let built = build_all(s, &vars)?;
    let launcher = write_launcher(s, &built)?;
    util::heading("Done");
    util::info(&format!("Start the game with: {}", launcher.display()));
    Ok(())
}

/// The two builds the port can make (`[build] cpu_variants`).
#[derive(Clone, Copy, PartialEq, Debug)]
enum Cpu {
    /// AVX2/FMA: the fast build.
    Avx2,
    /// SSE4.1 code on the x86-64-v2 runtime: any 64-bit CPU from about 2009 on.
    Compat,
}

impl Cpu {
    fn flavor(self) -> &'static str {
        match self {
            Cpu::Avx2 => "install",
            Cpu::Compat => "install-compat",
        }
    }
    fn set_vars(self, v: &mut Vars) {
        v.set("flavor", self.flavor());
        v.set("avx2", if self == Cpu::Avx2 { "ON" } else { "OFF" });
    }
    fn name(self) -> &'static str {
        match self {
            Cpu::Avx2 => "AVX2 (fast)",
            Cpu::Compat => "compatible (no AVX2)",
        }
    }
}

/// A finished build: which one, its exe and the placeholders it was built with.
struct Built {
    cpu: Cpu,
    exe: PathBuf,
    vars: Vars,
}

/// Which builds to make: --cpu, else the one this PC needs plus any an earlier install made
/// (so an update rebuilds both when both were installed).
fn cpu_targets(s: &Session, vars: &Vars) -> Result<Vec<Cpu>> {
    if !s.m.cpu_variants {
        return Ok(vec![Cpu::Avx2]);
    }
    let here = if util::cpu_has_avx2() { Cpu::Avx2 } else { Cpu::Compat };
    let mut v = match s.o.cpu.as_deref() {
        Some("avx2") => vec![Cpu::Avx2],
        Some("compat") => vec![Cpu::Compat],
        Some("both") => vec![here, if here == Cpu::Avx2 { Cpu::Compat } else { Cpu::Avx2 }],
        _ => vec![here],
    };
    if matches!(s.o.cpu.as_deref(), None | Some("auto")) {
        for other in [Cpu::Avx2, Cpu::Compat] {
            if v.contains(&other) {
                continue;
            }
            let mut ov = vars.clone();
            other.set_vars(&mut ov);
            if Path::new(&ov.expand(&s.m.build_exe)?).is_file() {
                util::info(&format!(
                    "An earlier install also built the {} version; rebuilding it too.",
                    other.name()
                ));
                v.push(other);
            }
        }
    }
    Ok(v)
}

/// Build every requested CPU variant.
fn build_all(s: &Session, vars: &Vars) -> Result<Vec<Built>> {
    let targets = cpu_targets(s, vars)?;
    if s.m.cpu_variants {
        util::info(&format!(
            "Processor: {}",
            if util::cpu_has_avx2() {
                "AVX2/FMA (the fast build runs here)"
            } else if util::cpu_has_compat_floor() {
                "no AVX2 (the compatible build runs here)"
            } else {
                "no SSE4.2/POPCNT: neither build of the game can run on this processor"
            }
        ));
        if targets.contains(&Cpu::Avx2) && !targets.contains(&Cpu::Compat) && !util::cpu_has_avx2() {
            util::warn("building only the AVX2 version, which this PC's processor cannot run");
        }
    }
    let mut out = Vec::new();
    let n = targets.len();
    for (i, cpu) in targets.into_iter().enumerate() {
        let mut v = vars.clone();
        cpu.set_vars(&mut v);
        if s.m.cpu_variants && n > 1 {
            util::heading(&format!("Building the {} version ({}/{n})", cpu.name(), i + 1));
        }
        let exe = build(s, &mut v)?;
        out.push(Built { cpu, exe, vars: v });
    }
    Ok(out)
}

fn build(s: &Session, vars: &mut Vars) -> Result<PathBuf> {
    util::heading("Compiler");
    let env = toolchain::detect_build_env()?;
    if let Some(vs) = &env.vs_path {
        util::info(&format!("Visual Studio: {}", vs.display()));
    }
    util::info(&format!("clang-cl/clang: {}", env.cc.display()));
    util::info(&format!(
        "CMake: {}  Ninja: {}",
        env.cmake.display(),
        env.ninja.display()
    ));
    vars.set_path("cmake", &env.cmake);
    vars.set_path("ninja", &env.ninja);
    vars.set_path("cc", &env.cc);
    vars.set_path("cxx", &env.cxx);
    let bj = s.o.build_jobs.unwrap_or_else(default_build_jobs);
    util::info(&format!("Compiling with {bj} parallel job(s)"));
    vars.set("build_jobs", bj.to_string());

    let (configure, compile, exe) = if cfg!(windows) {
        (&s.m.build_configure, &s.m.build_compile, &s.m.build_exe)
    } else {
        (
            &s.m.build_configure_unix,
            &s.m.build_compile_unix,
            &s.m.build_exe_unix,
        )
    };
    if configure.is_empty() || compile.is_empty() {
        bail!("the manifest has no build commands for this platform");
    }
    // CMake reads backslashes in -D values as escapes; give it forward slashes.
    let mut bvars = vars.clone();
    if cfg!(windows) {
        for (_, v) in bvars.0.iter_mut() {
            *v = v.replace('\\', "/");
        }
    }
    let mk = |argv: &[String]| -> Result<Command> {
        let argv = bvars.expand_all(argv)?;
        let mut c = Command::new(&argv[0]);
        c.args(&argv[1..])
            .current_dir(&s.root)
            .envs(env.env.iter().map(|(k, v)| (k.as_str(), v.as_str())));
        Ok(c)
    };
    run_build_steps(s, &s.m.prebuild, "Building the graphics plugin", &bvars, &env.env)?;
    util::heading("Configuring (CMake)");
    util::run(&mut mk(configure)?)?;
    util::heading("Compiling -- the recompiled code is huge; expect a long first build");
    let log = PathBuf::from(vars.expand(&s.m.build_log)?);
    toolchain::run_logged(&mut mk(compile)?, &log)?;
    let exe = PathBuf::from(vars.expand(exe)?);
    if !exe.is_file() {
        bail!("the build finished but {} is missing", exe.display());
    }
    run_build_steps(s, &s.m.postbuild, "Finishing the build", vars, &env.env)?;
    util::info(&format!("Built {}", exe.display()));
    Ok(exe)
}

/// `[[prebuild]]` / `[[postbuild]]` commands, each under its own heading.
fn run_build_steps(
    s: &Session,
    steps: &[manifest::Step],
    heading: &str,
    vars: &Vars,
    env: &[(String, String)],
) -> Result<()> {
    let n = steps.len();
    for (i, st) in steps.iter().enumerate() {
        util::heading(&format!("{heading} ({}/{n}): {}", i + 1, st.title));
        let log = match &st.log {
            Some(l) => Some(PathBuf::from(vars.expand(l)?)),
            None => None,
        };
        match pipeline::run_one_env(st, vars, &s.root, env, log.as_deref()) {
            Ok(()) => {}
            Err(e) if st.optional => util::warn(&format!("'{}' failed: {e}", st.id)),
            Err(e) => bail!("{}: {e}", st.title),
        }
    }
    Ok(())
}

/// (path, sha256, modified time) of every generated file present before regeneration.
fn snapshot_generated(
    s: &Session,
    lock: &lock::Lock,
) -> Result<Vec<(PathBuf, String, std::time::SystemTime)>> {
    let mut v = Vec::new();
    for e in &lock.files {
        let p = util::join_slash(&s.root, &e.path);
        if let Ok(md) = std::fs::metadata(&p) {
            if let (true, Ok(t)) = (md.is_file(), md.modified()) {
                v.push((p.clone(), hash::sha256_file(&p)?, t));
            }
        }
    }
    Ok(v)
}

/// Put back the old timestamp of every file whose content came out identical.
fn restore_mtimes(before: &[(PathBuf, String, std::time::SystemTime)]) -> usize {
    let mut n = 0;
    for (p, h, t) in before {
        if hash::sha256_file(p).map(|x| x == *h).unwrap_or(false) {
            if let Ok(f) = std::fs::OpenOptions::new().write(true).open(p) {
                if f.set_modified(*t).is_ok() {
                    n += 1;
                }
            }
        }
    }
    n
}

fn cmd_update(s: &Session) -> Result<()> {
    println!("{} PC port: update", s.m.game_title);
    println!("Folder: {}", s.root.display());
    let remote = update::Remote::new(&s.m, s.o.repo.as_deref(), s.o.branch.as_deref())?;
    let st = lock::State::load(&s.root.join(".installer").join("state"));
    if st.xex.is_none() && !s.o.check && !s.o.force {
        bail!(
            "this folder has not been installed yet (no .installer/state), so it may be a development tree whose\n    \
             edits an update would overwrite. Run the installer first, or pass --force to update anyway."
        );
    }

    util::heading("Checking for updates");
    util::info(&format!(
        "github.com/{}  (branch {})",
        remote.repo, remote.branch
    ));
    let plan = update::plan(&s.root, &s.m, &remote)?;
    let short = &plan.commit[..10];
    let pending_marker = s.root.join(".installer").join("update.pending");
    let pending = pending_marker.is_file();
    if plan.is_empty() {
        util::info(&format!("Already up to date (version {short})."));
    } else {
        util::info(&format!(
            "Version {short}: {} file(s) to download ({}), {} to remove [{}]",
            plan.fetch.len(),
            util::human_bytes(plan.download_bytes()),
            plan.delete.len(),
            update::summary(&plan)
        ));
    }
    if s.o.check {
        // One machine-readable line for the setup program.
        println!(
            "UPDATE-CHECK {} {} {} {}",
            if plan.is_empty() && !pending {
                "current"
            } else {
                "available"
            },
            plan.commit,
            plan.fetch.len(),
            plan.download_bytes()
        );
        return Ok(());
    }
    if plan.is_empty() && !pending {
        util::heading("Done");
        util::info("Nothing to do.");
        return Ok(());
    }

    if !plan.is_empty() {
        util::heading("Downloading the update");
        util::write_file(&pending_marker, plan.commit.as_bytes())?;
        update::apply(&s.root, &remote, &plan, &mut |l| util::info(&l))?;
        util::info(&format!("Files updated to version {short}."));
    } else {
        util::info(
            "The files are current, but the last update did not finish building; building now.",
        );
    }

    // Rebuild with the (possibly new) manifest and patch set. Regenerate only if the patch set's
    // results are no longer what is on disk; files that come out identical keep their timestamps.
    let m = Manifest::load(&s.installer_dir.join("fable2-installer.toml"))?;
    let lock = lock::Lock::load(&s.installer_dir.join("generated.lock"))?
        .ctx(|| "the update has no installer/generated.lock")?;
    let regen = !pipeline::lock_satisfied(&s.root, &lock)?;
    if regen {
        util::info("The port's patches changed: the game code is regenerated from your files (a few minutes).");
    }
    let o = Opts {
        cmd: "install".into(),
        root: Some(s.root.clone()),
        python: s.o.python.clone(),
        jobs: s.o.jobs,
        build_jobs: s.o.build_jobs,
        cpu: s.o.cpu.clone(),
        yes: true,
        regen,
        keep_mtimes: true,
        force: s.o.force,
        ..Default::default()
    };
    let s2 = Session {
        o,
        root: s.root.clone(),
        installer_dir: s.installer_dir.clone(),
        m,
    };
    cmd_install(&s2)?;
    let _ = std::fs::remove_file(&pending_marker);
    util::info(&format!("Updated to version {short}."));
    Ok(())
}

/// `Update <game>.cmd` next to the launcher: double-click to update.
fn write_update_script(s: &Session) -> Result<()> {
    if !cfg!(windows) || s.m.update_repo.is_none() {
        return Ok(());
    }
    let t = "@echo off\r\nrem Created by fable2-installer: downloads what changed on GitHub and rebuilds what that needs.\r\n\
             cd /d \"%~dp0\"\r\n\"installer\\fable2-installer.exe\" update\r\necho.\r\npause\r\n";
    util::write_file(
        &s.root.join(format!("Update {}.cmd", s.m.game_title)),
        t.as_bytes(),
    )
}

/// `Play <game>.cmd` for the build this PC runs, and `installer/launch.txt` for the launcher
/// (which picks between the builds itself at every start).
fn write_launcher(s: &Session, built: &[Built]) -> Result<PathBuf> {
    write_update_script(s)?;
    let primary = built
        .iter()
        .find(|b| b.cpu == Cpu::Avx2 && util::cpu_has_avx2())
        .or_else(|| built.iter().find(|b| b.cpu == Cpu::Compat))
        .or(built.first())
        .ctx(|| "nothing was built")?;
    write_launch_file(s, built)?;
    let vars = &primary.vars;
    let exe = &primary.exe;
    let cwd = vars.expand(&s.m.launch_cwd)?;
    let out = vars.expand(s.m.launch_stdout.as_deref().unwrap_or(""))?;
    let err = vars.expand(s.m.launch_stderr.as_deref().unwrap_or(""))?;
    let args = vars.expand_all(&s.m.launch_args)?;
    let mut redir = String::new();
    if !out.is_empty() {
        redir.push_str(&format!(" > \"{out}\""));
    }
    if !err.is_empty() {
        redir.push_str(&format!(" 2> \"{err}\""));
    }
    let quoted: String = args.iter().map(|a| format!(" \"{a}\"")).collect();
    if cfg!(windows) {
        let mut t = String::from("@echo off\r\nrem Created by fable2-installer. Re-run the installer to regenerate.\r\nsetlocal\r\n");
        t.push_str(&format!("cd /d \"{cwd}\"\r\n"));
        for (k, v) in &s.m.launch_env {
            t.push_str(&format!("set \"{k}={}\"\r\n", vars.expand(v)?));
        }
        t.push_str(&format!("\"{}\"{quoted}{redir}\r\n", exe.display()));
        t.push_str("if errorlevel 1 (\r\n  echo The game exited with an error. Its logs are in the logs folder next to it.\r\n  pause\r\n)\r\n");
        let p = s.root.join(format!("{}.cmd", s.m.launch_name));
        util::write_file(&p, t.as_bytes())?;
        Ok(p)
    } else {
        let mut t = String::from("#!/bin/sh\n# Created by fable2-installer.\n");
        t.push_str(&format!("cd \"{cwd}\" || exit 1\n"));
        for (k, v) in &s.m.launch_env {
            t.push_str(&format!("export {k}=\"{}\"\n", vars.expand(v)?));
        }
        t.push_str(&format!("exec \"{}\"{quoted}{redir}\n", exe.display()));
        let p = s.root.join(format!(
            "{}.sh",
            s.m.launch_name.replace(' ', "-").to_lowercase()
        ));
        util::write_file(&p, t.as_bytes())?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755));
        }
        Ok(p)
    }
}

/// `installer/launch.txt`: what the no-console launcher (`Fable II.exe`) starts. Paths inside the
/// checkout are stored relative to it, so the folder can be moved. The AVX2 build's lines have
/// plain keys (`exe=`, `cwd=`), the compatible build's a `_compat` suffix; the launcher uses the
/// AVX2 build when the processor has AVX2, else the compatible one.
fn write_launch_file(s: &Session, built: &[Built]) -> Result<()> {
    let rel = |p: &Path| {
        util::rel_slash(&util::absolute(&s.root), &util::absolute(p))
            .unwrap_or_else(|| util::path_str(p))
    };
    let mut t = String::from("# Written by fable2-installer; read by the launcher (Fable II.exe). Paths are relative to this checkout.\n");
    // Shared by every build: arguments, redirections, environment (from the first build's view).
    let first = &built.first().ctx(|| "nothing was built")?.vars;
    for a in first.expand_all(&s.m.launch_args)? {
        // Existing paths inside the checkout are stored relative; anything else as written.
        let p = Path::new(&a);
        if p.is_absolute() && p.exists() {
            t.push_str(&format!("arg={}\n", rel(p)));
        } else {
            t.push_str(&format!("argv={a}\n"));
        }
    }
    let out = first.expand(s.m.launch_stdout.as_deref().unwrap_or(""))?;
    let err = first.expand(s.m.launch_stderr.as_deref().unwrap_or(""))?;
    if !out.is_empty() {
        t.push_str(&format!("stdout={out}\n"));
    }
    if !err.is_empty() {
        t.push_str(&format!("stderr={err}\n"));
    }
    for (k, v) in &s.m.launch_env {
        t.push_str(&format!("env={k}={}\n", first.expand(v)?));
    }
    for b in built {
        let sfx = if b.cpu == Cpu::Compat { "_compat" } else { "" };
        let cwd = b.vars.expand(&s.m.launch_cwd)?;
        t.push_str(&format!("exe{sfx}={}\n", rel(&b.exe)));
        t.push_str(&format!("cwd{sfx}={}\n", rel(Path::new(&cwd))));
    }
    util::write_file(&s.installer_dir.join("launch.txt"), t.as_bytes())
}

/// Parallel compile jobs: the lifted translation units are huge, so roughly one job per 4 GB of RAM.
fn default_build_jobs() -> usize {
    let cpus = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4);
    match util::total_ram_bytes() {
        Some(r) => ((r / (4 << 30)) as usize).clamp(1, cpus.min(8)),
        None => 4.min(cpus),
    }
}

/// Copy a disc image's files (minus the firmware update folder) into `out`.
fn extract_iso_to(isop: &Path, out: &Path) -> Result<()> {
    let mut img = iso::open_ready(isop)?;
    let xex = img
        .find("default.xex")
        .ctx(|| "this disc image has no default.xex; it is not an Xbox 360 game disc")?;
    let head = img.read_head(&xex, 64 * 1024)?;
    let info = game::parse_xex(&head).ctx(|| "the disc's default.xex could not be read")?;
    img.skip_system_update();
    util::info(&format!(
        "Disc: {} [{}], media {}, {} files, {}",
        game::known_title(&info.title_id).unwrap_or("?"),
        info.title_id,
        info.media_id,
        img.files.len(),
        util::human_bytes(img.total_size()),
    ));
    let mut last = u64::MAX;
    img.extract(out, &mut |done, total, path| {
        let pct = if total == 0 { 100 } else { done * 100 / total };
        if pct != last {
            last = pct;
            println!("    extracting {pct}% {path}");
        }
    })?;
    util::info(&format!("Extracted to {}", out.display()));
    Ok(())
}

fn cmd_extract_iso(o: &Opts) -> Result<()> {
    let isop = o.iso.clone().ctx(|| "extract-iso needs --iso <file.iso>")?;
    let out = o.out.clone().ctx(|| "extract-iso needs --out <folder>")?;
    extract_iso_to(&isop, &out)
}

fn cmd_package(s: &Session) -> Result<()> {
    let out = s.o.out.clone().ctx(|| "package needs --out <setup.exe>")?;
    if lock::Lock::load(&s.installer_dir.join("generated.lock"))?.is_none() {
        bail!("installer/generated.lock is missing; run make-patches first");
    }
    let me = std::env::current_exe()?;
    let dir = me.parent().ctx(|| "no exe folder")?.to_path_buf();
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let stub = dir.join(format!("fable2-setup{ext}"));
    let launcher = dir.join(format!("fable2-launch{ext}"));
    for p in [&stub, &launcher] {
        if !p.is_file() {
            bail!(
                "{} is missing; build everything first: cargo build --release",
                p.display()
            );
        }
    }
    let mut skip = Vec::new();
    if let Some(l) = &s.m.link_name {
        skip.push(l.clone());
    }
    if let Some(r) = util::rel_slash(&util::absolute(&s.root), &util::absolute(&out)) {
        if let Some((d, _)) = r.rsplit_once('/') {
            skip.push(d.to_string());
        }
    }
    let files = pipeline::export_files(&s.m, &s.root, &skip)?;
    let mut list: Vec<(String, PathBuf)> = files
        .iter()
        .map(|f| (f.clone(), util::join_slash(&s.root, f)))
        .collect();
    list.push((format!("installer/fable2-installer{ext}"), me.clone()));
    list.push((format!("{}{ext}", s.m.game_title), launcher));
    let size = payload::write(&stub, &list, &out)?;
    util::info(&format!(
        "{} files -> {} ({})",
        list.len(),
        out.display(),
        util::human_bytes(size)
    ));
    util::info("Players need only this file and their disc image.");
    Ok(())
}

fn cmd_doctor(s: &Session) -> Result<()> {
    let mut ok = true;
    util::heading("Python");
    match toolchain::find_python(s.o.python.as_deref(), s.m.py_min) {
        Ok((p, v)) => util::info(&format!(
            "OK  Python {}.{} ({})",
            v.0,
            v.1,
            p.prog.join(" ")
        )),
        Err(e) => {
            ok = false;
            util::info(&format!("MISSING  {e}"));
        }
    }
    util::heading("Processor");
    if util::cpu_has_avx2() {
        util::info("OK  AVX2/FMA: the fast build runs on this PC");
    } else if util::cpu_has_compat_floor() {
        util::info("OK  no AVX2: the installer builds the compatible version (a little slower)");
    } else {
        ok = false;
        util::info("MISSING  SSE4.2/POPCNT: this processor is too old for the game's runtime");
    }
    util::heading("Compiler");
    match toolchain::detect_build_env() {
        Ok(env) => {
            if let Some(vs) = env.vs_path {
                util::info(&format!("OK  Visual Studio {}", vs.display()));
            }
            util::info(&format!("OK  {}", env.cc.display()));
            util::info(&format!("OK  {}", env.cmake.display()));
            util::info(&format!("OK  {}", env.ninja.display()));
        }
        Err(e) => {
            ok = false;
            util::info(&format!("MISSING  {e}"));
        }
    }
    util::heading("Patch set");
    match lock::Lock::load(&s.installer_dir.join("generated.lock"))? {
        Some(l) => util::info(&format!("OK  {} generated files described", l.files.len())),
        None => {
            ok = false;
            util::info("MISSING  installer/generated.lock (maintainer: run make-patches)");
        }
    }
    if s.o.game.is_some() {
        util::heading("Game");
        let g = s.game()?;
        s.check_title(&g)?;
        let lock = lock::Lock::load(&s.installer_dir.join("generated.lock"))?;
        match xex_hash_check(s, &g.xex, lock.as_ref()) {
            Ok(_) => util::info("OK  default.xex is the version the port was made from"),
            Err(e) => {
                ok = false;
                util::info(&format!("WRONG VERSION  {e}"));
            }
        }
    }
    println!();
    if ok {
        println!("Everything needed is installed.");
        Ok(())
    } else {
        bail!("some requirements are missing (see above)")
    }
}

fn cmd_make_patches(s: &Session) -> Result<()> {
    println!(
        "make-patches: rebuild installer/patches from the development tree at {}",
        s.root.display()
    );
    util::heading("Game files");
    let g = s.game()?;
    s.check_title(&g)?;
    let py = s.python()?;
    let sha = hash::sha256_file(&g.xex)?;
    util::info(&format!("default.xex sha256 {sha}"));

    let fresh = s.root.join(".installer").join("fresh");
    if fresh.exists() && !s.o.keep_fresh {
        util::info(&format!("Removing the previous {}", fresh.display()));
        if let Some(l) = &s.m.link_name {
            let link = fresh.join(l);
            if std::fs::symlink_metadata(&link).is_ok() {
                pipeline::remove_link(&link)?;
            }
        }
        std::fs::remove_dir_all(&fresh).ctx(|| format!("removing {}", fresh.display()))?;
    }
    if !fresh.exists() {
        util::heading("Exporting a clean tree (what a player downloads)");
        let (n, b) = pipeline::export_tree(&s.m, &s.root, &fresh)?;
        util::info(&format!(
            "{n} files, {} -> {}",
            util::human_bytes(b),
            fresh.display()
        ));
    }
    if let Some(l) = &s.m.link_name {
        pipeline::link_game(&fresh, l, &g.root)?;
    }
    let mut vars = pipeline::workspace_vars(&s.m, &fresh, &s.installer_dir);
    pipeline::game_vars(&mut vars, &g);
    vars.set_path("python", &py);
    vars.set("jobs", s.jobs().to_string());

    util::heading("Running the pipeline on the clean tree");
    let failed = pipeline::run_steps(&s.m, &fresh, &vars, &sha, s.o.regen)?;
    if !failed.is_empty() {
        util::warn(&format!("optional steps failed: {}", failed.join(", ")));
    }
    util::heading("Diffing the pipeline's output against your tree");
    let findings = pipeline::build_patch_set(&s.m, &fresh, &s.root, &s.installer_dir, &sha)?;
    if !findings.is_empty() {
        util::heading(&format!(
            "{} file(s) need attention before this patch set can be published",
            findings.len()
        ));
        for f in &findings {
            util::info(&format!("{}\n        {}", f.path, f.why));
        }
        if !s.o.allow_leaks {
            bail!("fix the steps above (or pass --allow-leaks to accept them knowingly)");
        }
    }
    util::heading("Done");
    util::info("Commit installer/generated.lock and installer/patches/ with the port.");
    util::info("The game-data patches (installer/data-patches/) are made separately; see installer/README.md.");
    Ok(())
}

fn cmd_export(s: &Session) -> Result<()> {
    let out = s.o.out.clone().ctx(|| "export needs --out <dir>")?;
    if out.exists()
        && std::fs::read_dir(&out)
            .map(|mut d| d.next().is_some())
            .unwrap_or(false)
    {
        bail!("{} is not empty", out.display());
    }
    let (n, b) = pipeline::export_tree(&s.m, &s.root, &out)?;
    util::info(&format!(
        "Exported {n} files ({}) to {}",
        util::human_bytes(b),
        out.display()
    ));
    util::info("Nothing matching [generated] or [export].exclude was copied. Review the tree before publishing.");
    Ok(())
}

fn real_main(o: Opts) -> Result<()> {
    if o.cmd == "help" {
        print!("{USAGE}");
        return Ok(());
    }
    if o.cmd == "extract-iso" {
        return cmd_extract_iso(&o);
    }
    let root = find_root(o.root.as_deref())?;
    let installer_dir = root.join("installer");
    let m = Manifest::load(&installer_dir.join("fable2-installer.toml"))?;
    let s = Session {
        o,
        root,
        installer_dir,
        m,
    };
    match s.o.cmd.as_str() {
        "install" => cmd_install(&s),
        "doctor" => cmd_doctor(&s),
        "update" => cmd_update(&s),
        "make-patches" => cmd_make_patches(&s),
        "export" => cmd_export(&s),
        "package" => cmd_package(&s),
        _ => unreachable!(),
    }
}

fn main() {
    let o = match parse_args() {
        Ok(o) => o,
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(2);
        }
    };
    let pause = o.interactive && cfg!(windows);
    let code = match real_main(o) {
        Ok(()) => 0,
        Err(e) => {
            eprintln!();
            eprintln!("ERROR: {e}");
            1
        }
    };
    if pause {
        util::prompt("\nPress Enter to close this window.");
    }
    std::process::exit(code);
}
