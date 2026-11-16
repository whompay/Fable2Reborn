"""Repair reviewed codegen misclassifications, never unknown/external branches."""
import argparse
import json
import re
from pathlib import Path

REVIEWED = {
    0x8299F6DC: 0x8299F6BC,
    0x830078BC: 0x8300792C, 0x830078D4: 0x83007920,
    0x830078EC: 0x83007920, 0x83007904: 0x83007920,
    0x8300791C: 0x8300792C,
    0x830079FC: 0x83007A6C, 0x83007A14: 0x83007A60,
    0x83007A2C: 0x83007A60, 0x83007A44: 0x83007A60,
    0x83007A5C: 0x83007A6C,
    0x830080E0: 0x83008168, 0x830080E8: 0x83008148,
    0x830080F0: 0x8300813C, 0x83008100: 0x83008124,
    0x8300814C: 0x8300815C, 0x8300816C: 0x8300817C,
}
BRANCH = re.compile(r'if \((!?ctx\.cr[0-7]\.(?:eq|lt|gt|so))\) REX_FATAL\("Unresolved branch from 0x([0-9A-F]{8}) to 0x([0-9A-F]{8})"\);')

def validate_instruction(word, source, target, condition):
    # bc, relative, no link, no CTR decrement: only a CR-bit test.
    bo, bi = (word >> 21) & 31, (word >> 16) & 31
    displacement = word & 0xFFFC
    if displacement & 0x8000:
        displacement -= 0x10000
    expected = ('!' if not bo & 8 else '') + f'ctx.cr{bi // 4}.' + ('lt', 'gt', 'eq', 'so')[bi % 4]
    if word >> 26 != 16 or word & 3 or bo not in (4, 5, 12, 13):
        raise ValueError(f'Unsupported branch instruction at {source:08X}')
    if source + displacement != target or condition != expected:
        raise ValueError(f'Original branch does not match emitted condition/target at {source:08X}')

def repair(text, image):
    changes = []
    parts = re.split(r'(?=DEFINE_REX_FUNC\()', text)
    for index, body in enumerate(parts):
        def replace(match):
            condition, source_hex, target_hex = match.groups()
            source, target = int(source_hex, 16), int(target_hex, 16)
            if REVIEWED.get(source) != target:
                raise ValueError(f'Unreviewed unresolved branch {source_hex} -> {target_hex}')
            if not body.startswith('DEFINE_REX_FUNC(') or not re.search(rf'^loc_{target_hex}:$', body, re.M):
                raise ValueError(f'Target {target_hex} is not local to the same function')
            offset = source - 0x82000000
            word = int.from_bytes(image[offset:offset + 4], 'big')
            validate_instruction(word, source, target, condition)
            changes.append({'source': source_hex, 'target': target_hex, 'instruction': f'{word:08X}'})
            return f'if ({condition}) goto loc_{target_hex}; // Verified local PPC branch'
        parts[index] = BRANCH.sub(replace, body)
    result = ''.join(parts)
    if 'REX_FATAL("Unresolved branch' in result:
        raise ValueError('Unsupported unresolved branch remains; review required')
    # Drop stale error comments only where a verified repair immediately follows.
    result = re.sub(r'\t// ERROR: conditional branch to unknown address 0x[0-9A-F]+\n(?=\tif .*// Verified local PPC branch)', '', result)
    return result, changes

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--project', type=Path, required=True)
    args = parser.parse_args()
    image = (args.project / 'default.pe').read_bytes()
    pending, evidence = [], []
    for file in sorted((args.project / 'generated/default').glob('*.cpp')):
        original = file.read_text()
        modified, changes = repair(original, image)
        if changes:
            pending.append((file, modified))
            evidence.extend(dict(file=file.name, **change) for change in changes)
    # Validate everything before any modification; do not rewrite unchanged files.
    for file, modified in pending:
        file.write_text(modified, newline='\n')
    if evidence:
        (args.project / 'local-branch-repairs.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f'Local branch validation passed; repaired {len(evidence)} branches in {len(pending)} files.')

if __name__ == '__main__':
    main()
