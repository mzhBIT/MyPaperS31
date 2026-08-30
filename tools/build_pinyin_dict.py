#!/usr/bin/env python3
"""Build MyPaperP4's sorted PYD1 pinyin dictionary.

Input lines: pinyin<TAB>candidate1 candidate2 ...
Output: fixed-size sorted lookup records followed by UTF-8 candidate blobs.
"""

import argparse
import pathlib
import struct

HEADER = struct.Struct("<4sII")
RECORD = struct.Struct("<32sIHH")


def normalize_key(value: str):
    # MyPaperP4 accepts continuous full pinyin and apostrophe-separated
    # syllables. pyim dictionaries use hyphens between every syllable.
    value = value.replace("-", "")
    return "".join(
        c.lower() for c in value.strip()
        if c.isascii() and (c.isalpha() or c == "'")
    )


def read_entries(path: pathlib.Path):
    merged = {}
    skipped = 0
    for raw in path.read_text(encoding="utf-8").splitlines():
        raw = raw.strip()
        if not raw or raw.startswith("#") or raw.startswith(";"):
            continue
        if "\t" in raw:
            key, values = raw.split("\t", 1)
        else:
            fields = raw.split(maxsplit=1)
            if len(fields) != 2:
                skipped += 1
                continue
            key, values = fields
        key = normalize_key(key)
        if not key or len(key.encode("ascii")) >= 32:
            skipped += 1
            continue
        words = merged.setdefault(key, [])
        for word in values.split():
            if word not in words:
                words.append(word)
    entries = [(key, " ".join(values).encode("utf-8")) for key, values in sorted(merged.items())]
    return entries, skipped


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    entries, skipped = read_entries(args.input)
    blob_offset = HEADER.size + RECORD.size * len(entries)
    records = []
    blobs = []
    cursor = blob_offset
    for key, values in entries:
        if len(values) > 8192:
            raise ValueError(f"candidate list too large: {key}")
        records.append(RECORD.pack(key.encode("ascii"), cursor, len(values), 0))
        blobs.append(values)
        cursor += len(values)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as out:
        out.write(HEADER.pack(b"PYD1", 1, len(entries)))
        out.writelines(records)
        out.writelines(blobs)
    print(f"wrote {len(entries)} entries, {cursor} bytes, skipped {skipped} lines -> {args.output}")


if __name__ == "__main__":
    main()
