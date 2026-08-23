"""Generate tightly cropped 1bpp Music UI icons from a 684x1216 SVG render.

Export Assets/Music.svg at its native size before running this script. The
search regions isolate each icon; the generator finds the actual ink bounds,
packs rows MSB first, and preserves the SVG control center as an anchor.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


THRESHOLD = 192

# enum name, C symbol, search bounds, logical anchor in the 684x1216 design
ICONS = (
    ("Previous", "icon_previous_data", (180, 960, 235, 1030), (207, 996)),
    ("Play", "icon_play_data", (310, 960, 375, 1030), (342, 996)),
    ("Pause", "icon_pause_data", (315, 1035, 370, 1090), (342, 1062)),
    ("Next", "icon_next_data", (450, 960, 505, 1030), (479, 996)),
    ("Shuffle", "icon_shuffle_data", (65, 1105, 120, 1165), (91, 1135)),
    ("Sequence", "icon_sequence_data", (180, 1105, 235, 1165), (207, 1134)),
    ("Playlist", "icon_playlist_data", (315, 1105, 370, 1165), (342, 1136)),
    ("VolumeDown", "icon_volume_down_data", (445, 1105, 505, 1165), (474, 1134)),
    ("VolumeUp", "icon_volume_up_data", (545, 1105, 615, 1165), (579, 1135)),
)


def ink_bounds(image: Image.Image, region: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    gray = image.crop(region).convert("L")
    xs: list[int] = []
    ys: list[int] = []
    for y in range(gray.height):
        for x in range(gray.width):
            if gray.getpixel((x, y)) < THRESHOLD:
                xs.append(region[0] + x)
                ys.append(region[1] + y)
    if not xs:
        raise RuntimeError(f"No icon pixels found in region {region}")
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def pack_1bpp(image: Image.Image, bounds: tuple[int, int, int, int]) -> tuple[bytes, int, int, int]:
    gray = image.crop(bounds).convert("L")
    width, height = gray.size
    stride = (width + 7) // 8
    packed = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            if gray.getpixel((x, y)) < THRESHOLD:
                packed[y * stride + x // 8] |= 0x80 >> (x & 7)
    return bytes(packed), width, height, stride


def format_array(symbol: str, data: bytes) -> str:
    lines = [f"const uint8_t {symbol}[] = {{"]
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset : offset + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def generate(input_path: Path, output_path: Path) -> None:
    image = Image.open(input_path).convert("RGB")
    if image.size != (684, 1216):
        raise RuntimeError(f"Expected a 684x1216 render, got {image.size}")

    assets = []
    for enum_name, symbol, region, target in ICONS:
        bounds = ink_bounds(image, region)
        data, width, height, stride = pack_1bpp(image, bounds)
        anchor_x = target[0] - bounds[0]
        anchor_y = target[1] - bounds[1]
        assets.append((enum_name, symbol, data, width, height, stride, anchor_x, anchor_y))

    out = [
        "// Generated from Assets/Music.svg; do not hand-edit bitmap data.",
        '#include "ui/music_icons_1bpp.h"',
        "",
        "namespace {",
        "",
    ]
    for _, symbol, data, *_ in assets:
        out.append(format_array(symbol, data))
        out.append("")
    out.extend(("}  // namespace", "", "namespace music_icons {", "", "namespace {", ""))
    out.append("const IconBitmap1bpp ICONS[] = {")
    for enum_name, symbol, data, width, height, stride, anchor_x, anchor_y in assets:
        out.append(
            f"    {{{symbol}, {width}, {height}, {stride}, sizeof({symbol}), {anchor_x}, {anchor_y}}},"
            f"  // {enum_name}"
        )
    out.extend(
        (
            "};",
            "",
            "static_assert(sizeof(ICONS) / sizeof(ICONS[0]) == static_cast<unsigned>(MusicIconId::Count));",
            "",
            "}  // namespace",
            "",
            "const IconBitmap1bpp *get(MusicIconId id)",
            "{",
            "    unsigned index = static_cast<unsigned>(id);",
            "    return index < static_cast<unsigned>(MusicIconId::Count) ? &ICONS[index] : nullptr;",
            "}",
            "",
            "}  // namespace music_icons",
            "",
        )
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(out), encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="Native-size PNG render of Assets/Music.svg")
    parser.add_argument("output", type=Path, help="Generated music_icons_1bpp.cpp")
    args = parser.parse_args()
    generate(args.input, args.output)


if __name__ == "__main__":
    main()
