#!/usr/bin/env python3
"""Repack gamescripts_r.bnk, replacing exactly one entry's payload with new
bytes (the patched generictriggers.lua bytecode) while leaving every other
entry's compressed bytes byte-for-byte identical to the original file.

Container format (documented in the project's glyph/chicken-suit research
docs, re-derived and verified here):
  Header (17 bytes): [4B base offset BE][4B const=3 BE][1B=1]
                      [4B packed catalog size BE][4B unpacked catalog size BE]
  Catalog (zlib, decompresses to `unpacked_cat` bytes): [4B row-count BE]
    then per entry: [4B namelen-incl-NUL BE][name+0x00]
                     [4B payload-offset BE][4B usize BE][4B psize BE]
                     [4B flag/chunk-count BE][flag*4B extras BE]
  Payloads: each at `base + offset`, zlib-compressed, `psize` bytes,
            padded out to the next 2-byte boundary between entries.

This script only ever touches the ONE named target entry's compressed
bytes; every other entry's compressed bytes are copied verbatim from the
original file (not re-decompressed/re-compressed), so nothing else in the
bank can drift from the original even by a single differing zlib byte.
"""
import struct, sys, zlib, hashlib

def read_catalog(cat_bytes):
    count = struct.unpack_from('>I', cat_bytes, 0)[0]
    p = 4
    entries = []
    for _ in range(count):
        namelen = struct.unpack_from('>I', cat_bytes, p)[0]; p += 4
        name_raw = cat_bytes[p:p+namelen]; p += namelen
        off, usize, psize, nflag = struct.unpack_from('>4I', cat_bytes, p); p += 16
        extras = list(struct.unpack_from(f'>{nflag}I', cat_bytes, p)) if nflag else []
        p += 4 * nflag
        entries.append(dict(name_raw=name_raw, off=off, usize=usize, psize=psize,
                             nflag=nflag, extras=extras))
    assert p == len(cat_bytes), (p, len(cat_bytes))
    return entries

def write_catalog(entries):
    out = bytearray()
    out += struct.pack('>I', len(entries))
    for e in entries:
        out += struct.pack('>I', len(e['name_raw']))
        out += e['name_raw']
        out += struct.pack('>4I', e['off'], e['usize'], e['psize'], e['nflag'])
        for x in e['extras']:
            out += struct.pack('>I', x)
    return bytes(out)

def main():
    bnk_path = sys.argv[1]
    target_name = sys.argv[2]       # e.g. 'scripts\\quests\\generictriggers.lua'
    new_payload_path = sys.argv[3]  # patched raw bytecode (uncompressed)
    out_path = sys.argv[4]

    b = bytearray(open(bnk_path, 'rb').read())
    orig_sha = hashlib.sha256(b).hexdigest()

    base = struct.unpack_from('>I', b, 0)[0]
    const3 = struct.unpack_from('>I', b, 4)[0]
    flag1 = b[8]
    packed_cat, unpacked_cat = struct.unpack_from('>II', b, 9)
    assert const3 == 3 and flag1 == 1

    cat_compressed = bytes(b[17:17+packed_cat])
    cat_bytes = zlib.decompressobj().decompress(cat_compressed)
    assert len(cat_bytes) == unpacked_cat
    entries = read_catalog(cat_bytes)
    print(f"parsed {len(entries)} catalog entries", file=sys.stderr)

    target_idx = None
    for i, e in enumerate(entries):
        if e['name_raw'].rstrip(b'\x00').decode('latin1') == target_name:
            target_idx = i
            break
    assert target_idx is not None, f"entry {target_name!r} not found"
    te = entries[target_idx]
    print(f"target entry: {target_name!r} off={te['off']} usize={te['usize']} "
          f"psize={te['psize']} nflag={te['nflag']} extras={te['extras']}", file=sys.stderr)
    assert te['nflag'] == 1, "target entry has an unexpected multi-chunk layout -- abort, needs review"

    new_payload = open(new_payload_path, 'rb').read()
    assert len(new_payload) == te['usize'], (
        f"patched payload length {len(new_payload)} != original usize {te['usize']} "
        f"-- the patch must not resize the bytecode")

    new_comp = zlib.compress(new_payload, 9)
    print(f"recompressed target: old psize={te['psize']} new psize={len(new_comp)}", file=sys.stderr)

    # Rebuild the payload region in original entry order, copying every
    # other entry's compressed bytes verbatim and substituting only the
    # target's.
    payload = bytearray()
    new_offsets = {}
    for i, e in enumerate(entries):
        if i == target_idx:
            comp = new_comp
        else:
            start = base + e['off']
            comp = bytes(b[start:start+e['psize']])
        new_off = len(payload)
        payload += comp
        if len(payload) % 2 != 0:
            payload += b'\x00'
        new_offsets[i] = new_off

    for i, e in enumerate(entries):
        e['off'] = new_offsets[i]
    entries[target_idx]['psize'] = len(new_comp)
    # usize/extras unchanged (bytecode length didn't change)

    new_cat_bytes = write_catalog(entries)
    assert len(new_cat_bytes) == unpacked_cat, (
        f"serialized catalog length {len(new_cat_bytes)} != original {unpacked_cat} "
        f"-- entry count/name/field-width must be unchanged")
    new_cat_compressed = zlib.compress(new_cat_bytes, 9)
    assert len(new_cat_compressed) <= (base - 17), (
        "new compressed catalog no longer fits before the fixed payload base offset")

    out = bytearray()
    out += struct.pack('>I', base)
    out += struct.pack('>I', const3)
    out += bytes([flag1])
    out += struct.pack('>II', len(new_cat_compressed), len(new_cat_bytes))
    out += new_cat_compressed
    out += b'\x00' * (base - len(out))
    out += payload

    open(out_path, 'wb').write(out)
    new_sha = hashlib.sha256(out).hexdigest()
    print(f"orig bank sha256={orig_sha} size={len(b)}", file=sys.stderr)
    print(f"new  bank sha256={new_sha} size={len(out)}", file=sys.stderr)

if __name__ == '__main__':
    main()
