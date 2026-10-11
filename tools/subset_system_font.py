"""Generate the firmware UI TTF subset. Requires fonttools (PC only).

Run after UI/localization edits; no fontTools dependency on the ESP-IDF build.
The generated manifest records provenance and missing glyphs for review.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STRINGS = re.compile(r'"(?:[^"\\]|\\.)*"')


def ui_characters():
    # Include every main C/C++ literal: localized strings, keyboard candidates,
    # diagnostics and dynamic-format punctuation. Never scan the old LV bitmap.
    chars = set(chr(cp) for cp in range(32, 127))
    chars.update('□…°×÷−√π²³∞≤≥≠≈←→↑↓♥简体中文繁體中文')
    for path in sorted((ROOT / 'main').rglob('*')):
        if path.suffix.lower() not in ('.c', '.cpp', '.h', '.hpp'):
            continue
        for literal in STRINGS.findall(path.read_text(encoding='utf-8-sig')):
            # Keep Unicode text literal; decode only C's explicit Unicode escapes.
            literal = re.sub(r'\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8}))',
                             lambda match: chr(int(match[1] or match[2], 16)), literal)
            # UTF-8 byte escapes are used by a few text producers.
            literal = re.sub(r'(?:\\x[0-9a-fA-F]{2})+',
                             lambda match: bytes(int(h, 16) for h in
                                 re.findall(r'\\x([0-9a-fA-F]{2})', match[0]))
                                 .decode('utf-8', errors='ignore'), literal)
            chars.update(c for c in literal if ord(c) >= 32)
    return chars


def main():
    from fontTools.ttLib import TTFont
    from fontTools.subset import Options, Subsetter
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--src', type=Path, default=ROOT / 'Assets/pingxianzhensong.ttf')
    parser.add_argument('--out', type=Path, default=ROOT / 'main/assets/system-ui.ttf')
    parser.add_argument('--check', action='store_true', help='check current UI coverage without writing')
    args = parser.parse_args()
    source = args.out if args.check else args.src
    font = TTFont(source, recalcTimestamp=False)
    wanted = ui_characters()
    available = font.getBestCmap()
    missing = sorted(ord(c) for c in wanted if ord(c) not in available)
    if args.check:
        print(f'UI subset coverage: {len(wanted)} requested, {len(missing)} unsupported')
        manifest = json.loads(args.out.with_suffix('.json').read_text(encoding='utf-8'))
        unexpected = set(missing) - set(manifest['missing_codepoints'])
        if unexpected:
            raise SystemExit('Regenerate subset; missing: ' + ', '.join(f'U+{cp:04X}' for cp in sorted(unexpected)))
        return
    options = Options()
    options.recalc_timestamp = False
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.layout_features = ['*']
    subsetter = Subsetter(options=options)
    subsetter.populate(unicodes=sorted(ord(c) for c in wanted if ord(c) in available))
    subsetter.subset(font)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    font.save(args.out)
    metadata = {
        'source': args.src.name, 'source_sha256': hashlib.sha256(args.src.read_bytes()).hexdigest(),
        'requested': len(wanted), 'retained': len(font.getBestCmap()),
        'bytes': args.out.stat().st_size, 'missing_codepoints': missing,
        'license': 'Source font retains its original license; redistribution rights must be verified before release.',
    }
    args.out.with_suffix('.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(metadata, ensure_ascii=False))


if __name__ == '__main__':
    main()
