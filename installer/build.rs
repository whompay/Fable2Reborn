//! Embeds the app icon (assets/fable2.ico) in the Windows executables, so Explorer, shortcuts and
//! the taskbar show it. Uses llvm-rc (part of the Visual Studio clang component the port already
//! needs) or the Windows SDK's rc.exe. If neither is found the build still succeeds, only without
//! the file icon (the launcher window draws its icon itself, see src/brand.rs).

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn candidates() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    if let Ok(rc) = env::var("RC") {
        v.push(rc.into());
    }
    v.push("llvm-rc".into());
    v.push("rc".into());
    let pf = env::var("ProgramFiles").unwrap_or_else(|_| r"C:\Program Files".into());
    let pf86 = env::var("ProgramFiles(x86)").unwrap_or_else(|_| r"C:\Program Files (x86)".into());
    v.push(Path::new(&pf).join(r"LLVM\bin\llvm-rc.exe"));
    for root in [&pf, &pf86] {
        for year in ["2022", "2019"] {
            for ed in ["BuildTools", "Community", "Professional", "Enterprise"] {
                v.push(
                    Path::new(root)
                        .join(r"Microsoft Visual Studio")
                        .join(year)
                        .join(ed)
                        .join(r"VC\Tools\Llvm\x64\bin\llvm-rc.exe"),
                );
            }
        }
    }
    // Windows SDK rc.exe: newest version folder first.
    let kits = Path::new(&pf86).join(r"Windows Kits\10\bin");
    if let Ok(rd) = std::fs::read_dir(&kits) {
        let mut vers: Vec<PathBuf> = rd.flatten().map(|e| e.path()).collect();
        vers.sort();
        for d in vers.into_iter().rev() {
            v.push(d.join(r"x64\rc.exe"));
        }
    }
    v
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=assets/fable2.rc");
    println!("cargo:rerun-if-changed=assets/fable2.ico");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows")
        || env::var("CARGO_CFG_TARGET_ENV").as_deref() != Ok("msvc")
    {
        return;
    }
    let assets = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("assets");
    let res = PathBuf::from(env::var("OUT_DIR").unwrap()).join("fable2.res");
    for tool in candidates() {
        let ran = Command::new(&tool)
            .current_dir(&assets)
            .arg("/fo")
            .arg(&res)
            .arg("fable2.rc")
            .output();
        if let Ok(o) = ran {
            if o.status.success() && res.is_file() {
                println!("cargo:rustc-link-arg-bins={}", res.display());
                return;
            }
        }
    }
    println!("cargo:warning=No resource compiler (llvm-rc / rc.exe) found: the executables are built without a file icon.");
}
