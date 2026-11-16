//! Xbox 360 disc images: the XDVDFS ("XISO") file system, read without any third-party code.
//!
//! Handles full Redump-style images (the game partition starts at 0xFD90000 on XGD2 discs,
//! 0x2080000 on XGD3, 0x18300000 on XGD1) and trimmed XISO images (partition at 0). The volume
//! descriptor ("MICROSOFT*XBOX*MEDIA") sits 32 sectors into the partition; directory tables are
//! binary trees of entries `left:u16 right:u16 sector:u32 size:u32 attr:u8 namelen:u8 name`
//! (little-endian, offsets in 4-byte units), padded with 0xFF at sector ends.
//!
//! Xbox 360 game data on these images is not encrypted, so no disc key is needed.

use crate::bail;
use crate::util::{self, Ctx, Result};
use std::collections::HashSet;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

pub const SECTOR: u64 = 2048;
const MAGIC: &[u8; 20] = b"MICROSOFT*XBOX*MEDIA";
/// Game partition offsets: XGD2, XGD2 variant, XGD3, XGD1, trimmed XISO.
const PARTITIONS: [u64; 5] = [0x0FD9_0000, 0x0FDA_0000, 0x0208_0000, 0x1830_0000, 0];
/// The firmware update folder on retail discs; not needed to play.
pub const SYSTEM_UPDATE: &str = "$SystemUpdate/";

#[derive(Clone, Debug)]
pub struct Entry {
    /// Path inside the image with '/' separators, as stored (case preserved).
    pub path: String,
    /// Absolute byte offset of the file's data in the image.
    pub offset: u64,
    pub size: u64,
}

pub struct Iso {
    file: fs::File,
    pub image_size: u64,
    /// Byte offset of the game partition.
    pub partition: u64,
    pub files: Vec<Entry>,
    /// Folders (for creating empty ones on extraction).
    pub dirs: Vec<String>,
}

fn u16le(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
fn u32le(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn clean_name(raw: &[u8]) -> Option<String> {
    let s = std::str::from_utf8(raw).ok()?;
    if s.is_empty()
        || s == "."
        || s == ".."
        || s.chars().any(|c| matches!(c, '/' | '\\' | ':' | '\0') || (c as u32) < 32)
    {
        return None;
    }
    Some(s.to_string())
}

impl Iso {
    pub fn open(path: &Path) -> Result<Iso> {
        let mut file = fs::File::open(path).ctx(|| format!("opening {}", path.display()))?;
        let image_size = file.metadata()?.len();
        let mut partition = None;
        let mut root = (0u32, 0u32);
        for p in PARTITIONS {
            let at = p + 32 * SECTOR;
            if at + 28 > image_size {
                continue;
            }
            let mut h = [0u8; 28];
            file.seek(SeekFrom::Start(at))?;
            file.read_exact(&mut h)?;
            if &h[..20] == MAGIC {
                partition = Some(p);
                root = (u32le(&h, 20), u32le(&h, 24));
                break;
            }
        }
        let Some(partition) = partition else {
            bail!(
                "{} is not an Xbox 360 / Xbox disc image (no XDVDFS volume found). \
                 Use a 1:1 (Redump) or XISO image of the game disc.",
                path.display()
            );
        };
        let mut iso = Iso {
            file,
            image_size,
            partition,
            files: Vec::new(),
            dirs: Vec::new(),
        };
        let mut seen = HashSet::new();
        iso.walk(root.0, root.1, "", 0, &mut seen)?;
        if iso.files.is_empty() {
            bail!("{} contains no files", path.display());
        }
        Ok(iso)
    }

    fn walk(
        &mut self,
        sector: u32,
        size: u32,
        prefix: &str,
        depth: u32,
        seen: &mut HashSet<u32>,
    ) -> Result<()> {
        if size == 0 || depth > 32 || !seen.insert(sector) {
            return Ok(());
        }
        let at = self.partition + sector as u64 * SECTOR;
        if at + size as u64 > self.image_size {
            bail!("the disc image is truncated (a folder table lies past its end)");
        }
        let mut table = vec![0u8; size as usize];
        self.file.seek(SeekFrom::Start(at))?;
        self.file.read_exact(&mut table)?;
        let mut stack = vec![0usize];
        let mut visited = HashSet::new();
        let mut subdirs = Vec::new();
        while let Some(off) = stack.pop() {
            if off + 14 > table.len() || !visited.insert(off) {
                continue;
            }
            let left = u16le(&table, off);
            if left == 0xFFFF {
                continue; // padding
            }
            let right = u16le(&table, off + 2);
            let start = u32le(&table, off + 4);
            let len = u32le(&table, off + 8);
            let attr = table[off + 12];
            let nlen = table[off + 13] as usize;
            if nlen == 0 || off + 14 + nlen > table.len() {
                continue;
            }
            let Some(name) = clean_name(&table[off + 14..off + 14 + nlen]) else {
                bail!("the disc image has an unreadable file name in {prefix:?}");
            };
            let path = format!("{prefix}{name}");
            if attr & 0x10 != 0 {
                self.dirs.push(path.clone());
                subdirs.push((start, len, format!("{path}/")));
            } else {
                let offset = self.partition + start as u64 * SECTOR;
                if offset + len as u64 > self.image_size {
                    bail!("the disc image is truncated ({path} lies past its end)");
                }
                self.files.push(Entry {
                    path,
                    offset,
                    size: len as u64,
                });
            }
            if left != 0 {
                stack.push(left as usize * 4);
            }
            if right != 0 {
                stack.push(right as usize * 4);
            }
        }
        for (s, l, p) in subdirs {
            self.walk(s, l, &p, depth + 1, seen)?;
        }
        Ok(())
    }

    /// Case-insensitive lookup ('/' or '\' separators).
    pub fn find(&self, path: &str) -> Option<Entry> {
        let want = path.replace('\\', "/").trim_matches('/').to_ascii_lowercase();
        self.files
            .iter()
            .find(|e| e.path.to_ascii_lowercase() == want)
            .cloned()
    }

    pub fn read_head(&mut self, e: &Entry, max: u64) -> Result<Vec<u8>> {
        let n = e.size.min(max) as usize;
        let mut buf = vec![0u8; n];
        self.file.seek(SeekFrom::Start(e.offset))?;
        self.file.read_exact(&mut buf)?;
        Ok(buf)
    }

    pub fn read_file(&mut self, e: &Entry) -> Result<Vec<u8>> {
        self.read_head(e, e.size)
    }

    pub fn total_size(&self) -> u64 {
        self.files.iter().map(|e| e.size).sum()
    }

    /// Drop the firmware update folder (not needed to play).
    pub fn skip_system_update(&mut self) {
        let low = SYSTEM_UPDATE.to_ascii_lowercase();
        self.files
            .retain(|e| !e.path.to_ascii_lowercase().starts_with(&low));
        self.dirs.retain(|d| {
            let d = format!("{}/", d.to_ascii_lowercase());
            !d.starts_with(&low)
        });
    }

    /// Copy every file into `out`; `progress(done_bytes, total_bytes, current_path)`.
    pub fn extract(&mut self, out: &Path, progress: &mut dyn FnMut(u64, u64, &str)) -> Result<()> {
        fs::create_dir_all(out).ctx(|| format!("creating {}", out.display()))?;
        for d in &self.dirs {
            let p = util::join_slash(out, d);
            fs::create_dir_all(&p).ctx(|| format!("creating {}", p.display()))?;
        }
        let total = self.total_size();
        let mut done = 0u64;
        let mut buf = vec![0u8; 4 << 20];
        let files = self.files.clone();
        for e in &files {
            let dst = util::join_slash(out, &e.path);
            // Already there with the right size (an interrupted copy is resumed file by file).
            if fs::metadata(&dst).map(|m| m.len() == e.size).unwrap_or(false) {
                done += e.size;
                progress(done, total, &e.path);
                continue;
            }
            util::ensure_parent(&dst)?;
            let tmp = dst.with_extension("part~");
            {
                let mut w = fs::File::create(&tmp).ctx(|| format!("creating {}", tmp.display()))?;
                self.file.seek(SeekFrom::Start(e.offset))?;
                let mut left = e.size;
                while left > 0 {
                    let n = left.min(buf.len() as u64) as usize;
                    self.file
                        .read_exact(&mut buf[..n])
                        .ctx(|| format!("reading {} from the disc image", e.path))?;
                    w.write_all(&buf[..n])
                        .ctx(|| format!("writing {}", tmp.display()))?;
                    left -= n as u64;
                    done += n as u64;
                    progress(done, total, &e.path);
                }
                w.flush()?;
            }
            fs::rename(&tmp, &dst).ctx(|| format!("renaming {}", tmp.display()))?;
        }
        progress(total, total, "");
        Ok(())
    }
}

/// Open an image and check that it has a readable file system.
pub fn open_ready(path: &Path) -> Result<Iso> {
    Iso::open(path)
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// Build a tiny XDVDFS image: partition at `base`, root folder with `default.xex` and
    /// `data/a.bin`.
    pub fn tiny_image(base: u64, files: &[(&str, &[u8])]) -> Vec<u8> {
        // layout (sectors relative to the partition): 32 = volume descriptor, 40 = root table,
        // 41 = data table, 50.. = file data
        let mut img = vec![0u8; (base + 80 * SECTOR) as usize];
        let put = |img: &mut Vec<u8>, at: u64, b: &[u8]| {
            let at = at as usize;
            if img.len() < at + b.len() {
                img.resize(at + b.len(), 0);
            }
            img[at..at + b.len()].copy_from_slice(b);
        };
        let vd = base + 32 * SECTOR;
        put(&mut img, vd, MAGIC);
        let mut next_data = 50u64;
        let mut root_entries: Vec<(String, u32, u32, u8)> = Vec::new();
        let mut data_entries: Vec<(String, u32, u32, u8)> = Vec::new();
        for (name, bytes) in files {
            put(&mut img, base + next_data * SECTOR, bytes);
            let e = (name.rsplit('/').next().unwrap().to_string(), next_data as u32, bytes.len() as u32, 0x80u8);
            if name.contains('/') {
                data_entries.push(e);
            } else {
                root_entries.push(e);
            }
            next_data += (bytes.len() as u64).div_ceil(SECTOR).max(1);
        }
        // a linear chain via `right`
        let table = |entries: &[(String, u32, u32, u8)]| -> Vec<u8> {
            let mut t = Vec::new();
            for (i, (n, s, l, a)) in entries.iter().enumerate() {
                let start = t.len();
                let len = 14 + n.len();
                let next = (start + len).div_ceil(4) * 4;
                let right = if i + 1 < entries.len() { (next / 4) as u16 } else { 0 };
                t.extend_from_slice(&0u16.to_le_bytes());
                t.extend_from_slice(&right.to_le_bytes());
                t.extend_from_slice(&s.to_le_bytes());
                t.extend_from_slice(&l.to_le_bytes());
                t.push(*a);
                t.push(n.len() as u8);
                t.extend_from_slice(n.as_bytes());
                while t.len() < next {
                    t.push(0xFF);
                }
            }
            t
        };
        let dt = table(&data_entries);
        root_entries.push(("data".into(), 41, dt.len() as u32, 0x10));
        let rt = table(&root_entries);
        put(&mut img, base + 40 * SECTOR, &rt);
        put(&mut img, base + 41 * SECTOR, &dt);
        let mut d = Vec::new();
        d.extend_from_slice(&40u32.to_le_bytes());
        d.extend_from_slice(&(rt.len() as u32).to_le_bytes());
        put(&mut img, vd + 20, &d);
        img
    }

    #[test]
    fn reads_and_extracts() {
        for base in [0u64, 0x0208_0000] {
            let img = tiny_image(base, &[("default.xex", b"XEX2hello"), ("data/a.bin", b"abc")]);
            let dir = std::env::temp_dir().join(format!("f2i-iso-{}-{base}", std::process::id()));
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            let p = dir.join("t.iso");
            fs::write(&p, &img).unwrap();
            let mut iso = open_ready(&p).unwrap();
            assert_eq!(iso.partition, base);
            assert_eq!(iso.files.len(), 2);
            let e = iso.find("DATA\\A.BIN").unwrap();
            assert_eq!(iso.read_file(&e).unwrap(), b"abc");
            let out = dir.join("out");
            iso.extract(&out, &mut |_, _, _| {}).unwrap();
            assert_eq!(fs::read(out.join("default.xex")).unwrap(), b"XEX2hello");
            assert_eq!(fs::read(out.join("data").join("a.bin")).unwrap(), b"abc");
            let _ = fs::remove_dir_all(&dir);
        }
    }
}
