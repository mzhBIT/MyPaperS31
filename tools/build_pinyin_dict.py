#!/usr/bin/env python3
"""Build MyPaperP4's sorted PYD1 pinyin dictionary.

Input lines: pinyin<TAB>candidate1 candidate2 ...
Output: fixed-size sorted lookup records, UTF-8 candidate blobs, and an
optional-compatible PY91 nine-key index trailer. Older firmware ignores the
trailer and continues to read the PYD1 section normally.
"""

import argparse
import pathlib
import struct

HEADER = struct.Struct("<4sII")
RECORD = struct.Struct("<32sIHH")
T9_HEADER = struct.Struct("<4sII")
T9_RECORD = struct.Struct("<32s")


def t9_digits(key: str):
    result = []
    for char in key:
        if char in "abc":
            result.append("2")
        elif char in "def":
            result.append("3")
        elif char in "ghi":
            result.append("4")
        elif char in "jkl":
            result.append("5")
        elif char in "mno":
            result.append("6")
        elif char in "pqrs":
            result.append("7")
        elif char in "tuv":
            result.append("8")
        elif char in "wxyz":
            result.append("9")
        elif char == "'":
            result.append("0")
    return "".join(result)


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
    t9_records = []
    cursor = blob_offset
    for source_index, (key, values) in enumerate(entries):
        if len(values) > 8192:
            raise ValueError(f"candidate list too large: {key}")
        records.append(RECORD.pack(key.encode("ascii"), cursor, len(values), 0))
        blobs.append(values)
        t9_records.append((t9_digits(key), key, source_index))
        cursor += len(values)
    t9_records.sort()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as out:
        out.write(HEADER.pack(b"PYD1", 1, len(entries)))
        out.writelines(records)
        out.writelines(blobs)
        out.write(T9_HEADER.pack(b"PY92", 2, len(t9_records)))
        for _digits, key, _source_index in t9_records:
            out.write(T9_RECORD.pack(key.encode("ascii")))
    final_size = cursor + T9_HEADER.size + T9_RECORD.size * len(t9_records)
    print(
        f"wrote {len(entries)} entries, {final_size} bytes "
        f"(PYD1={cursor}, PY92={final_size - cursor}), skipped {skipped} lines "
        f"-> {args.output}"
    )


if __name__ == "__main__":
    main()
