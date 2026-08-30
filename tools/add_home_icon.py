"""Pack a 150x150 monochrome PNG into the project's home icon table."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


WIDTH = 150
HEIGHT = 150
STRIDE = 19
THRESHOLD = 192


def pack(path: Path) -> bytes:
    image = Image.open(path).convert("L")
    if image.size != (WIDTH, HEIGHT):
        raise RuntimeError(f"expected {WIDTH}x{HEIGHT}, got {image.size}")
    result = bytearray(STRIDE * HEIGHT)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if image.getpixel((x, y)) < THRESHOLD:
                result[y * STRIDE + x // 8] |= 0x80 >> (x & 7)
    return bytes(result)


def format_array(symbol: str, data: bytes) -> str:
    lines = [f"const uint8_t {symbol}[HOME_ICON_1BPP_DATA_SIZE] = {{"]
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{value:02X}" for value in data[offset : offset + 16])
        lines.append(f"    {values},")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--cpp", type=Path)
    parser.add_argument("--enum")
    args = parser.parse_args()
    array = format_array(args.symbol, pack(args.input))
    args.output.write_text(array + "\n", encoding="utf-8")
    if args.cpp is not None:
        if not args.enum:
            raise RuntimeError("--enum is required with --cpp")
        source = args.cpp.read_text(encoding="utf-8")
        if f"const uint8_t {args.symbol}[" not in source:
            source = source.replace("\n} // namespace\n", f"\n{array}\n\n}} // namespace\n", 1)
        switch_case = f"    case HomeIconId::{args.enum}: return {args.symbol};\n"
        if switch_case not in source:
            source = source.replace("    default: return nullptr;\n", switch_case + "    default: return nullptr;\n", 1)
        args.cpp.write_text(source, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
