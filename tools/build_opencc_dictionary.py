#!/usr/bin/env python3
"""Build small, read-only OpenCC dictionaries for the reader's PSRAM loader.

Usage: python tools/build_opencc_dictionary.py OPENCC/data/dictionary OUTPUT_DIR
The input is an unmodified OpenCC source checkout (Apache-2.0).  This tool
does not embed OpenCC data in the firmware or modify the source checkout.
"""

import argparse
import pathlib
import struct
import subprocess
import zlib

HEADER = struct.Struct("<8sIIIIIII")
ENTRY = struct.Struct("<IIII")
MAGIC = b"MPOCC01\0"
SOURCES = {
    "s2t": ("STPhrases.txt", "STPhrases_GeneratedFromRegionalPhrases.txt", "STCharacters.txt"),
    "t2s": ("TSPhrases.txt", "TSCharactersExt.txt", "TSCharacters.txt"),
}
OPTIONAL = {"STPhrases_GeneratedFromRegionalPhrases.txt", "TSCharactersExt.txt"}


def load_entries(source_dir, names):
    result = {}
    for name in names:
        path = source_dir / name
        if not path.is_file():
            if name in OPTIONAL:
                print(f"Optional generated dictionary not present: {path}")
                continue
            raise FileNotFoundError(f"Required OpenCC dictionary missing: {path}")
        for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t", 1)
            if len(fields) != 2 or not fields[0] or not fields[1].strip():
                raise ValueError(f"Invalid dictionary row: {path}:{line_number}")
            # OpenCC text dictionaries can list alternative output candidates;
            # the standard conversion selects their first candidate.
            value = fields[1].strip().split(" ", 1)[0]
            result.setdefault(fields[0], value)
    return result


def build(entries, direction):
    data = bytearray()
    records = bytearray()
    max_codepoints = 1
    for key, value in sorted(entries.items(), key=lambda item: item[0].encode("utf-8")):
        key_bytes = key.encode("utf-8")
        value_bytes = value.encode("utf-8")
        key_offset = len(data)
        data.extend(key_bytes)
        value_offset = len(data)
        data.extend(value_bytes)
        records.extend(ENTRY.pack(key_offset, value_offset, len(key_bytes), len(value_bytes)))
        max_codepoints = max(max_codepoints, len(key))
    payload = records + data
    header = HEADER.pack(MAGIC, 1, direction, len(entries), max_codepoints,
                         len(records), len(data), zlib.crc32(payload))
    return header + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="OpenCC data/dictionary directory")
    parser.add_argument("output", type=pathlib.Path, help="Directory for s2t.bin and t2s.bin")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        revision = subprocess.run(
            ["git", "-C", str(args.source), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=False,
        )
        source_revision = revision.stdout.strip() if revision.returncode == 0 else "unknown"
    except FileNotFoundError:
        source_revision = "unknown"
    for direction, names in SOURCES.items():
        entries = load_entries(args.source, names)
        blob = build(entries, 1 if direction == "s2t" else 2)
        path = args.output / f"{direction}.bin"
        path.write_bytes(blob)
        print(f"{path}: {len(entries)} entries, {len(blob)} bytes, CRC32 checked")
    (args.output / "SOURCE.txt").write_text(
        "Dictionary source: https://github.com/BYVoid/OpenCC\n"
        f"OpenCC revision: {source_revision}\n"
        "Dictionary license: Apache-2.0; retain upstream notices when distributing.\n"
        "Conversion profile: phrase-priority + single-character fallback; not exact OpenCC runtime parity.\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
