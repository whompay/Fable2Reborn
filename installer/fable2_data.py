#!/usr/bin/env python3
"""Fable II PC port: the port's changes to the game's own data files.

The port runs from the player's extracted disc (`extracted/`). A few of the game's files are
changed or added there: keyboard/mouse button prompts baked into copies of the GUI texture
banks (`data/art/_gui_kbm`, `data/art/_gui_x360`, `data/art/gui/...`), and small Lua bytecode
edits inside `data/gamescripts_r.bnk` (camera, Ancestors' Chest rewards). None of those files can
be published, so the maintainer records only the *differences*, and the installer applies them to
the player's own copy:

  maintainer:  python fable2_data.py make  --iso "<the game's .iso>" --tree ../extracted --out data-patches
  installer:   python fable2_data.py apply --game ../extracted --patches data-patches --backup <dir>
  either:      python fable2_data.py check --game ../extracted --patches data-patches

`make` reads the original files straight from the disc image and refuses to write a patch set
that would carry a large part of any original file (that would be publishing game data).

Patch kinds (data-patches/manifest.json):
  copy      the target is an unchanged copy of an original file
  same      the target is byte-identical to another target
  ranges    byte ranges of the target that differ from its original (the port's own art), plus
            the final size; the bytes are in a .bin file next to the manifest
  bank      `gamescripts_r.bnk`-style container: per entry, the byte ranges of the *decompressed*
            payload that differ (the port's bytecode edits). The bank is rebuilt: unchanged
            entries keep their compressed bytes, changed ones are re-compressed (zlib level 9).

`apply` is idempotent and keeps a copy of every original file it overwrites in --backup, so it
can always start again from the originals.
"""
import argparse
import difflib
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

FORMAT = 1
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITIONS = (0x0FD90000, 0x0FDA0000, 0x02080000, 0x18300000, 0)

# (target, original) -- paths relative to the extracted disc, '/' separators.
GUI = ["gui.gdb", "gui.list", "gui_models.bnk", "gui_streaming.bnk",
       "gui_texture_headers.bnk", "gui_textures.bnk"]
DEFAULT_TARGETS = (
    [(f"data/art/_gui_x360/{f}", f"data/art/gui/{f}") for f in GUI]
    + [(f"data/art/_gui_kbm/{f}", f"data/art/gui/{f}") for f in GUI]
    + [
        ("data/art/gui/gui_textures_x360.bnk", "data/art/gui/gui_textures.bnk"),
        ("data/art/gui/gui_textures_kbm.bnk", "data/art/gui/gui_textures.bnk"),
        ("data/art/gui/gui_textures.bnk", "data/art/gui/gui_textures.bnk"),
        ("data/art/gui/gui_texture_headers.bnk", "data/art/gui/gui_texture_headers.bnk"),
        ("data/gamescripts_r.bnk", "data/gamescripts_r.bnk"),
    ]
)
MERGE_GAP = 16
MAX_SHARE = 0.5  # a patch may not carry more than this share of its original


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def say(*a):
    print(*a, flush=True)


# ---- disc image (XDVDFS) -------------------------------------------------------------------

class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        size = self.f.seek(0, 2)
        self.base = None
        for p in PARTITIONS:
            at = p + 32 * SECTOR
            if at + 28 > size:
                continue
            self.f.seek(at)
            h = self.f.read(28)
            if h[:20] == MAGIC:
                self.base = p
                root = struct.unpack_from("<II", h, 20)
                break
        if self.base is None:
            raise SystemExit(f"{path}: no Xbox 360 file system (XDVDFS) found")
        self.files = {}
        self._walk(root[0], root[1], "", set())

    def _walk(self, sector, size, prefix, seen):
        if not size or sector in seen:
            return
        seen.add(sector)
        self.f.seek(self.base + sector * SECTOR)
        t = self.f.read(size)
        stack, visited, subdirs = [0], set(), []
        while stack:
            o = stack.pop()
            if o + 14 > len(t) or o in visited:
                continue
            visited.add(o)
            left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", t, o)
            if left == 0xFFFF or nlen == 0 or o + 14 + nlen > len(t):
                continue
            name = t[o + 14:o + 14 + nlen].decode("ascii", "replace")
            path = prefix + name
            if attr & 0x10:
                subdirs.append((start, length, path + "/"))
            else:
                self.files[path.lower()] = (self.base + start * SECTOR, length)
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)
        for s, l, p in subdirs:
            self._walk(s, l, p, seen)

    def read(self, rel):
        e = self.files.get(rel.lower())
        if e is None:
            raise SystemExit(f"the disc image has no {rel}")
        self.f.seek(e[0])
        return self.f.read(e[1])


# ---- byte ranges ---------------------------------------------------------------------------

def diff_ranges(a, b, gap=MERGE_GAP):
    """[(offset, length)] where b differs from a (b's bytes past len(a) included)."""
    out = []
    n = min(len(a), len(b))
    block = 4096
    i = 0
    while i < n:
        j = min(i + block, n)
        if a[i:j] == b[i:j]:
            i = j
            continue
        k = i
        while k < j:
            if a[k] != b[k]:
                s = k
                while k < j and a[k] != b[k]:
                    k += 1
                if out and s - (out[-1][0] + out[-1][1]) <= gap:
                    out[-1][1] = k - out[-1][0]
                else:
                    out.append([s, k - s])
            else:
                k += 1
        i = j
    if len(b) > n:
        if out and n - (out[-1][0] + out[-1][1]) <= gap:
            out[-1][1] = len(b) - out[-1][0]
        else:
            out.append([n, len(b) - n])
    return out


def apply_ranges(base, ranges, blob, final_size):
    out = bytearray(base[:final_size])
    if len(out) < final_size:
        out.extend(b"\0" * (final_size - len(out)))
    p = 0
    for off, ln in ranges:
        out[off:off + ln] = blob[p:p + ln]
        p += ln
    if p != len(blob):
        raise ValueError("patch data does not match its ranges")
    return bytes(out)


def blob_of(b, ranges):
    return b"".join(b[o:o + l] for o, l in ranges)


def delta(a, b):
    """Ops turning a into b: ["c", a_offset, length] copies, ["i", length] inserts (bytes in the
    returned blob). For payloads whose size changed."""
    ops, blob = [], bytearray()
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            ops.append(["c", i1, i2 - i1])
        elif j2 > j1:
            ops.append(["i", j2 - j1])
            blob += b[j1:j2]
    return ops, bytes(blob)


def apply_delta(a, ops, blob):
    out, p = bytearray(), 0
    for op in ops:
        if op[0] == "c":
            out += a[op[1]:op[1] + op[2]]
        else:
            out += blob[p:p + op[1]]
            p += op[1]
    if p != len(blob):
        raise ValueError("patch data does not match its ops")
    return bytes(out)


# ---- gamescripts_r.bnk-style banks ---------------------------------------------------------
# Header: [u32 base][u32 3][u8 1][u32 packed catalog][u32 catalog]; zlib catalog at 17;
# catalog: [u32 count] then per entry [u32 namelen][name][u32 off][u32 usize][u32 psize][u32 n][n*u32];
# payloads (zlib) at base+off, each padded to 2 bytes.  (tools/repack_gamescripts_bank.py)

class Bank:
    def __init__(self, b):
        self.raw = b
        self.base, c3 = struct.unpack_from(">II", b, 0)
        flag = b[8]
        packed, unpacked = struct.unpack_from(">II", b, 9)
        if c3 != 3 or flag != 1:
            raise ValueError("not a gamescripts-style bank")
        cat = zlib.decompressobj().decompress(b[17:17 + packed])
        if len(cat) != unpacked:
            raise ValueError("catalog size mismatch")
        count = struct.unpack_from(">I", cat, 0)[0]
        p = 4
        self.entries = []
        for _ in range(count):
            nl = struct.unpack_from(">I", cat, p)[0]
            p += 4
            name = cat[p:p + nl]
            p += nl
            off, usize, psize, nflag = struct.unpack_from(">4I", cat, p)
            p += 16
            extras = list(struct.unpack_from(f">{nflag}I", cat, p)) if nflag else []
            p += 4 * nflag
            self.entries.append(dict(name=name, off=off, usize=usize, psize=psize,
                                     nflag=nflag, extras=extras))
        if p != len(cat):
            raise ValueError("catalog has trailing bytes")
        self.catalog_len = unpacked

    def key(self, e):
        return e["name"].rstrip(b"\0").decode("latin1")

    def compressed(self, e):
        s = self.base + e["off"]
        return self.raw[s:s + e["psize"]]

    def payload(self, e):
        if e["nflag"] != 1:
            return None  # multi-chunk entries are copied verbatim, never patched
        d = zlib.decompressobj().decompress(self.compressed(e))
        return d if len(d) == e["usize"] else None

    def rebuild(self, new_payloads):
        """new_payloads: {name: bytes}; the entries keep their order, the rest their bytes."""
        payload = bytearray()
        entries = [dict(e) for e in self.entries]
        for e, o in zip(entries, self.entries):
            k = self.key(o)
            if k in new_payloads:
                data = new_payloads[k]
                comp = zlib.compress(data, 9)
                e["psize"] = len(comp)
                if e["extras"] == [e["usize"]]:
                    e["extras"] = [len(data)]  # single chunk: its size is the payload size
                e["usize"] = len(data)
            else:
                comp = self.compressed(o)
            e["off"] = len(payload)
            payload += comp
            if len(payload) % 2:
                payload += b"\0"
        cat = bytearray(struct.pack(">I", len(entries)))
        for e in entries:
            cat += struct.pack(">I", len(e["name"])) + e["name"]
            cat += struct.pack(">4I", e["off"], e["usize"], e["psize"], e["nflag"])
            for x in e["extras"]:
                cat += struct.pack(">I", x)
        if len(cat) != self.catalog_len:
            raise ValueError("rebuilt catalog changed size")
        cc = zlib.compress(bytes(cat), 9)
        if 17 + len(cc) > self.base:
            raise ValueError("rebuilt catalog does not fit before the payloads")
        out = bytearray(struct.pack(">II", self.base, 3) + b"\x01" + struct.pack(">II", len(cc), len(cat)))
        out += cc
        out += b"\0" * (self.base - len(out))
        out += payload
        return bytes(out)


def same_content(a, b):
    """Two banks with the same catalog and the same decompressed payloads (the compressed bytes
    may differ when another zlib build compressed them)."""
    try:
        x, y = Bank(a), Bank(b)
    except Exception:
        return False
    if len(x.entries) != len(y.entries):
        return False
    for ex, ey in zip(x.entries, y.entries):
        if (x.key(ex), ex["usize"], ex["nflag"], ex["extras"]) != (y.key(ey), ey["usize"], ey["nflag"], ey["extras"]):
            return False
        if x.compressed(ex) == y.compressed(ey):
            continue
        px, py = x.payload(ex), y.payload(ey)
        if px is None or px != py:
            return False
    return True


def is_bank(b):
    try:
        Bank(b)
        return True
    except Exception:
        return False


# ---- make (maintainer) ---------------------------------------------------------------------

def cmd_make(a):
    disc = Disc(a.iso)
    tree = Path(a.tree)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    targets = DEFAULT_TARGETS
    originals = {}
    entries = []
    finals = {}
    problems = []
    total_blob = 0
    for n, (target, base) in enumerate(targets):
        tp = tree / target
        if not tp.is_file():
            problems.append(f"{target}: missing in {tree}")
            continue
        new = tp.read_bytes()
        if base not in originals:
            originals[base] = disc.read(base)
        orig = originals[base]
        fsha = sha256(new)
        rec = {"target": target, "base": base, "base_sha256": sha256(orig),
               "final_sha256": fsha, "final_size": len(new)}
        dup = next((t for t, h in finals.items() if h == fsha), None)
        if new == orig:
            rec["kind"] = "copy"
            say(f"copy    {target}")
        elif dup:
            rec["kind"] = "same"
            rec["same_as"] = dup
            say(f"same    {target} = {dup}")
        else:
            done = False
            if is_bank(orig) and is_bank(new):
                bo, bn = Bank(orig), Bank(new)
                same_layout = [bo.key(e) for e in bo.entries] == [bn.key(e) for e in bn.entries]
                if same_layout:
                    changed = {}
                    ok = True
                    for eo, en in zip(bo.entries, bn.entries):
                        if bo.compressed(eo) == bn.compressed(en):
                            continue
                        po, pn = bo.payload(eo), bn.payload(en)
                        if po is None or pn is None:
                            ok = False
                            break
                        if po != pn:
                            changed[bo.key(eo)] = (po, pn)
                    rebuilt = bo.rebuild({k: v[1] for k, v in changed.items()}) if ok else None
                    if ok and (rebuilt == new or same_content(rebuilt, new)):
                        if rebuilt != new:
                            say(f"note    {target}: this Python's zlib compresses differently; the content is verified")
                        blobs, ents = bytearray(), []
                        for k, (po, pn) in changed.items():
                            e = {"name": k, "usize": len(pn), "base_sha256": sha256(po),
                                 "final_sha256": sha256(pn), "blob_offset": len(blobs)}
                            if len(po) == len(pn):
                                r = diff_ranges(po, pn)
                                data = blob_of(pn, r)
                                e["ranges"] = r
                            else:
                                e["ops"], data = delta(po, pn)
                            e["blob_size"] = len(data)
                            ents.append(e)
                            blobs += data
                        rec["kind"] = "bank"
                        rec["entries"] = ents
                        rec["blob"] = f"{n:02d}.bin"
                        (out / rec["blob"]).write_bytes(bytes(blobs))
                        total_blob += len(blobs)
                        share = len(blobs) / max(1, len(orig))
                        say(f"bank    {target}: {len(ents)} entr{'y' if len(ents) == 1 else 'ies'} changed, "
                            f"{len(blobs)} bytes of edits ({share:.2%} of the original)")
                        for e in ents:
                            say(f"          {e['name']}: {e['blob_size']} bytes")
                        if share > MAX_SHARE:
                            problems.append(f"{target}: the edits carry {share:.0%} of the original file")
                        done = True
                    elif ok:
                        say(f"note    {target}: rebuilding the bank does not reproduce it; using byte ranges")
            if not done:
                r = diff_ranges(orig, new)
                data = blob_of(new, r)
                rec["kind"] = "ranges"
                rec["ranges"] = r
                rec["blob"] = f"{n:02d}.bin"
                (out / rec["blob"]).write_bytes(data)
                total_blob += len(data)
                share = len(data) / max(1, len(orig))
                say(f"ranges  {target}: {len(r)} range(s), {len(data)} bytes ({share:.2%} of the original)")
                if share > MAX_SHARE:
                    problems.append(f"{target}: the patch carries {share:.0%} of the original file")
        finals[target] = fsha
        entries.append(rec)

    # self-check: rebuild every target from the originals
    files = {}
    for rec in entries:
        files[rec["target"]] = build_target(rec, originals[rec["base"]], files, out)
        if sha256(files[rec["target"]]) != rec["final_sha256"]:
            if rec["kind"] == "bank" and same_content(files[rec["target"]], (tree / rec["target"]).read_bytes()):
                continue
            problems.append(f"{rec['target']}: the patch does not reproduce the file")
    manifest = {"format": FORMAT, "xex_note": "made from the disc's original files", "targets": entries}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    used = {r["blob"] for r in entries if "blob" in r}
    for old in sorted(out.glob("*.bin")):
        if old.name not in used:
            try:
                old.unlink()
            except OSError:
                say(f"note: {old} is no longer used; delete it")
    say(f"\n{len(entries)} target(s), {total_blob} bytes of patch data -> {out}")
    if problems:
        say("\nPROBLEMS:\n  " + "\n  ".join(problems))
        if not a.allow_leaks:
            raise SystemExit("the patch set is not safe to publish (see above); fix it or pass --allow-leaks")


def build_target(rec, base, done, pdir):
    kind = rec["kind"]
    if kind == "copy":
        return base
    if kind == "same":
        return done[rec["same_as"]]
    blob = (Path(pdir) / rec["blob"]).read_bytes()
    if kind == "ranges":
        return apply_ranges(base, rec["ranges"], blob, rec["final_size"])
    if kind == "bank":
        b = Bank(base)
        by_name = {b.key(e): e for e in b.entries}
        new = {}
        for e in rec["entries"]:
            src = b.payload(by_name[e["name"]])
            if src is None or sha256(src) != e["base_sha256"]:
                raise ValueError(f"{rec['base']}: entry {e['name']} is not the original")
            part = blob[e["blob_offset"]:e["blob_offset"] + e["blob_size"]]
            if "ops" in e:
                p = apply_delta(src, e["ops"], part)
            else:
                p = apply_ranges(src, e["ranges"], part, e["usize"])
            if sha256(p) != e["final_sha256"]:
                raise ValueError(f"{rec['target']}: entry {e['name']} came out wrong")
            new[e["name"]] = p
        return b.rebuild(new)
    raise ValueError(f"unknown patch kind {kind}")


def bank_matches(rec, data):
    """A rebuilt bank whose compressed bytes differ (another zlib) but whose content is right."""
    try:
        b = Bank(data)
    except Exception:
        return False
    want = {e["name"]: e["final_sha256"] for e in rec["entries"]}
    for e in b.entries:
        k = b.key(e)
        if k in want:
            p = b.payload(e)
            if p is None or sha256(p) != want[k]:
                return False
    return True


# ---- apply / check (installer) -------------------------------------------------------------

def load_manifest(pdir):
    m = json.loads((Path(pdir) / "manifest.json").read_text(encoding="utf-8"))
    if m.get("format") != FORMAT:
        raise SystemExit(f"{pdir}/manifest.json has an unknown format")
    return m["targets"]


def is_final(rec, data):
    if sha256(data) == rec["final_sha256"]:
        return True
    return rec["kind"] == "bank" and len(data) > 17 and bank_matches(rec, data)


def cmd_check(a):
    game = Path(a.game)
    bad = 0
    for rec in load_manifest(a.patches):
        p = game / rec["target"]
        ok = p.is_file() and is_final(rec, p.read_bytes())
        if not ok:
            bad += 1
        say(f"{'OK     ' if ok else 'PENDING'} {rec['target']}")
    if bad:
        raise SystemExit(f"{bad} file(s) are not in their final state")


def write_atomic(dst, data):
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".part~")
    tmp.write_bytes(data)
    os.replace(tmp, dst)


def cmd_apply(a):
    game = Path(a.game)
    pdir = Path(a.patches)
    backup = Path(a.backup)
    targets = load_manifest(pdir)

    todo = []
    for rec in targets:
        p = game / rec["target"]
        if p.is_file() and is_final(rec, p.read_bytes()):
            say(f"already done  {rec['target']}")
        else:
            todo.append(rec["target"])
    if not todo:
        say("All game-data changes are in place.")
        return

    base_sha = {}
    for rec in targets:
        base_sha[rec["base"]] = rec["base_sha256"]

    # 1. Keep a copy of every original file this run is going to overwrite.
    for t in todo:
        if t in base_sha:
            src, dst = game / t, backup / t
            if src.is_file() and not dst.is_file() and sha256(src.read_bytes()) == base_sha[t]:
                write_atomic(dst, src.read_bytes())
                say(f"kept the original {t} in {backup}")

    # 2. The originals the pending targets are made from.
    originals = {}

    def original(base):
        if base not in originals:
            for src in (game / base, backup / base):
                if src.is_file():
                    d = src.read_bytes()
                    if sha256(d) == base_sha[base]:
                        originals[base] = d
                        break
            else:
                raise SystemExit(
                    f"{game / base} is not the game's original file, and no original copy is in {backup}.\n"
                    f"Copy the game from the disc again (or restore that file), then run the installer again.")
        return originals[base]

    # 3. Build and write, in manifest order ("same" targets refer to earlier ones).

    class Done(dict):
        def __missing__(self, k):
            v = (game / k).read_bytes()
            self[k] = v
            return v

    built = Done()
    for rec in targets:
        t = rec["target"]
        if t not in todo:
            continue
        data = build_target(rec, original(rec["base"]) if rec["kind"] != "same" else b"", built, pdir)
        if sha256(data) != rec["final_sha256"]:
            if rec["kind"] == "bank" and bank_matches(rec, data):
                say(f"note: {t} was re-compressed by a different zlib; its content is verified")
            else:
                raise SystemExit(f"{t}: the patch result is wrong (the patch set is damaged)")
        write_atomic(game / t, data)
        built[t] = data
        say(f"{rec['kind']:<7} {t}")
    say(f"Game-data changes applied: {len(todo)} file(s) written.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make", help="(maintainer) record the port's changes against the disc's original files")
    m.add_argument("--iso", required=True)
    m.add_argument("--tree", required=True, help="the development tree's extracted/ folder")
    m.add_argument("--out", required=True)
    m.add_argument("--allow-leaks", action="store_true")
    p = sub.add_parser("apply", help="apply the changes to the player's extracted/ folder")
    p.add_argument("--game", required=True)
    p.add_argument("--patches", required=True)
    p.add_argument("--backup", required=True, help="where originals that get overwritten are kept")
    c = sub.add_parser("check", help="report which files are in their final state")
    c.add_argument("--game", required=True)
    c.add_argument("--patches", required=True)
    a = ap.parse_args()
    {"make": cmd_make, "apply": cmd_apply, "check": cmd_check}[a.cmd](a)


if __name__ == "__main__":
    main()
