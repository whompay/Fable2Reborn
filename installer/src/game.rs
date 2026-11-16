//! The player's game files: locating the extracted disc (the folder that holds `default.xex`),
//! reading the XEX2 header (title ID, media ID, version), and small byte-level helpers the
//! pipeline can use.

use crate::bail;
use crate::util::{self, Ctx, Result};
use std::fs;
use std::path::{Path, PathBuf};

pub struct Game {
    /// Folder that holds `default.xex` and `data/` (the root of the extracted disc).
    pub root: PathBuf,
    /// `<root>/default.xex`.
    pub xex: PathBuf,
    pub info: XexInfo,
}

impl Game {
    pub fn title_id(&self) -> &str {
        &self.info.title_id
    }
    /// A readable name: XEX files carry no title string, so this is the port's name for the
    /// title ID when it is known, else the ID itself.
    pub fn title(&self) -> &str {
        known_title(&self.info.title_id).unwrap_or(&self.info.title_id)
    }
}

/// Names for the title IDs this installer is likely to be shown.
pub fn known_title(title_id: &str) -> Option<&'static str> {
    match title_id {
        "4D5307F1" => Some("Fable II"),
        "4D5309C9" => Some("Fable II (Game of the Year)"),
        "4D5307D1" => Some("Fable III"),
        _ => None,
    }
}

/// What the XEX2 execution-info header says about the executable.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct XexInfo {
    /// 8 hex digits, upper case, e.g. "4D5307F1".
    pub title_id: String,
    /// 8 hex digits, upper case.
    pub media_id: String,
    /// a.b.c.d
    pub version: String,
    pub disc: u8,
    pub discs: u8,
}

fn u32be(d: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_be_bytes(d.get(o..o + 4)?.try_into().ok()?))
}

fn version_str(v: u32) -> String {
    // XEX version: major:4 minor:4 build:16 qfe:8
    format!("{}.{}.{}.{}", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF)
}

/// Parse the header of an XEX2 file (only the first few KB are needed).
pub fn parse_xex(d: &[u8]) -> Option<XexInfo> {
    if d.get(0..4)? != b"XEX2" {
        return None;
    }
    let count = u32be(d, 0x14)? as usize;
    for i in 0..count.min(256) {
        let e = 0x18 + i * 8;
        let key = u32be(d, e)?;
        let val = u32be(d, e + 4)? as usize;
        // 0x00040006: execution info (24 bytes at `val`).
        if key == 0x0004_0006 {
            let media = u32be(d, val)?;
            let version = u32be(d, val + 4)?;
            let title = u32be(d, val + 12)?;
            let disc = *d.get(val + 18)?;
            let discs = *d.get(val + 19)?;
            return Some(XexInfo {
                title_id: format!("{title:08X}"),
                media_id: format!("{media:08X}"),
                version: version_str(version),
                disc,
                discs,
            });
        }
    }
    None
}

pub fn read_xex_info(path: &Path) -> Result<XexInfo> {
    use std::io::Read;
    let mut f = fs::File::open(path).ctx(|| format!("opening {}", path.display()))?;
    let mut head = vec![0u8; 64 * 1024];
    let n = f.read(&mut head)?;
    head.truncate(n);
    parse_xex(&head).ctx(|| {
        format!(
            "{} is not an Xbox 360 executable (no XEX2 header with a title ID)",
            path.display()
        )
    })
}

/// Accepts the extracted disc folder, a folder above it that holds `extracted/`, or
/// `default.xex` itself.
pub fn locate(input: &Path) -> Result<Game> {
    let input = util::absolute(input);
    let mut cands: Vec<PathBuf> = Vec::new();
    let mut p: Option<&Path> = Some(&input);
    for _ in 0..4 {
        let Some(d) = p else { break };
        cands.push(d.to_path_buf());
        cands.push(d.join("extracted"));
        p = d.parent();
    }
    for c in &cands {
        let xex = c.join("default.xex");
        if xex.is_file() {
            if !c.join("data").is_dir() {
                bail!(
                    "{} has default.xex but no data folder; copy the whole disc, not just the executable",
                    c.display()
                );
            }
            let info = read_xex_info(&xex)?;
            return Ok(Game {
                root: c.clone(),
                xex,
                info,
            });
        }
    }
    bail!(
        "could not find default.xex in or above {}.\n    Point the installer at the folder with your extracted \
         disc (the one that contains default.xex and data), or use the disc image (.iso) itself.",
        input.display()
    )
}

/// Copy `len` bytes at `offset` of `src` into `dst`.
pub fn carve(src: &Path, offset: u64, len: u64, dst: &Path) -> Result<()> {
    let d = fs::read(src).ctx(|| format!("reading {}", src.display()))?;
    let (o, l) = (offset as usize, len as usize);
    if o.checked_add(l).map(|e| e > d.len()).unwrap_or(true) {
        bail!(
            "carve {:#x}+{:#x} is past the end of {} ({} bytes)",
            offset,
            len,
            src.display(),
            d.len()
        );
    }
    util::write_file(dst, &d[o..o + l])
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal XEX2 header: one optional header (execution info).
    pub fn fake_xex(title: u32, media: u32, version: u32) -> Vec<u8> {
        let mut d = vec![0u8; 0x200];
        d[0..4].copy_from_slice(b"XEX2");
        d[0x14..0x18].copy_from_slice(&1u32.to_be_bytes());
        d[0x18..0x1C].copy_from_slice(&0x0004_0006u32.to_be_bytes());
        d[0x1C..0x20].copy_from_slice(&0x100u32.to_be_bytes());
        d[0x100..0x104].copy_from_slice(&media.to_be_bytes());
        d[0x104..0x108].copy_from_slice(&version.to_be_bytes());
        d[0x10C..0x110].copy_from_slice(&title.to_be_bytes());
        d[0x112] = 1;
        d[0x113] = 1;
        d
    }

    #[test]
    fn xex_header() {
        let d = fake_xex(0x4D53_07F1, 0x716F_0A0D, 0x0000_1A00);
        let i = parse_xex(&d).unwrap();
        assert_eq!(i.title_id, "4D5307F1");
        assert_eq!(i.media_id, "716F0A0D");
        assert_eq!(i.version, "0.0.26.0");
        assert_eq!((i.disc, i.discs), (1, 1));
        assert!(parse_xex(b"XEX1....").is_none());
    }

    #[test]
    fn locate_finds_extracted() {
        let t = std::env::temp_dir().join(format!("f2i-locate-{}", std::process::id()));
        let ex = t.join("extracted");
        fs::create_dir_all(ex.join("data")).unwrap();
        fs::write(ex.join("default.xex"), fake_xex(0x4D53_07F1, 1, 0)).unwrap();
        let g = locate(&t).unwrap();
        assert_eq!(g.title_id(), "4D5307F1");
        assert_eq!(g.title(), "Fable II");
        assert_eq!(g.root, util::absolute(&ex));
        let _ = fs::remove_dir_all(&t);
    }
}
