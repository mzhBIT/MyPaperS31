"""Generate tightly cropped 1bpp Music UI icons.

Export Assets/Music.svg at its native size before running this script. The
search regions isolate each icon; the generator finds the actual ink bounds,
packs rows MSB first, and preserves the SVG control center as an anchor.

Playback-mode icons can be replaced from Apple SymbolKit SVG exports. Only the
Regular-S path and its optical margin/capline box are rasterized; Notes and
Guides never enter the bitmap.
"""

from __future__ import annotations

import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageDraw


THRESHOLD = 192

# enum name, C symbol, search bounds, logical anchor in the 684x1216 design,
# optional SymbolKit source file
ICONS = (
    ("Previous", "icon_previous_data", (180, 960, 235, 1030), (207, 996), None),
    ("Play", "icon_play_data", (310, 960, 375, 1030), (342, 996), None),
    ("Pause", "icon_pause_data", (315, 1035, 370, 1090), (342, 1062), None),
    ("Next", "icon_next_data", (450, 960, 505, 1030), (479, 996), None),
    ("Shuffle", "icon_shuffle_data", (65, 1105, 120, 1165), (91, 1135), "shuffle.svg"),
    ("Sequence", "icon_sequence_data", (180, 1105, 235, 1165), (207, 1134), "repeat.svg"),
    ("RepeatOne", "icon_repeat_one_data", None, None, "repeat-1.svg"),
    ("Snowflake", "icon_snowflake_data", None, None, "snowflake.svg"),
    ("Playlist", "icon_playlist_data", (315, 1105, 370, 1165), (342, 1136), None),
    ("VolumeDown", "icon_volume_down_data", (445, 1105, 505, 1165), (474, 1134), None),
    ("VolumeUp", "icon_volume_up_data", (545, 1105, 615, 1165), (579, 1135), None),
)

PATH_TOKEN = re.compile(r"[MmLlHhVvCc]|[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")
SYMBOL_CANVAS = (46, 36)
SYMBOL_OVERSAMPLE = 8


def _svg_node(root: ET.Element, node_id: str) -> ET.Element:
    for node in root.iter():
        if node.attrib.get("id") == node_id:
            return node
    raise RuntimeError(f"SVG node not found: {node_id}")


def _flatten_path(path_data: str, curve_steps: int = 18) -> list[list[tuple[float, float]]]:
    tokens = PATH_TOKEN.findall(path_data)
    paths: list[list[tuple[float, float]]] = []
    current_path: list[tuple[float, float]] = []
    x = y = 0.0
    command = ""
    index = 0

    def is_command(value: str) -> bool:
        return len(value) == 1 and value.isalpha()

    def number() -> float:
        nonlocal index
        if index >= len(tokens) or is_command(tokens[index]):
            raise RuntimeError(f"Malformed path near token {index}")
        value = float(tokens[index])
        index += 1
        return value

    while index < len(tokens):
        if is_command(tokens[index]):
            command = tokens[index]
            index += 1
        if command == "M":
            if current_path:
                paths.append(current_path)
            x, y = number(), number()
            current_path = [(x, y)]
            command = "L"
        elif command == "L":
            x, y = number(), number()
            current_path.append((x, y))
        elif command == "l":
            x, y = x + number(), y + number()
            current_path.append((x, y))
        elif command == "h":
            x += number()
            current_path.append((x, y))
        elif command == "v":
            y += number()
            current_path.append((x, y))
        elif command == "c":
            x0, y0 = x, y
            x1, y1 = x + number(), y + number()
            x2, y2 = x + number(), y + number()
            x3, y3 = x + number(), y + number()
            for step in range(1, curve_steps + 1):
                t = step / curve_steps
                u = 1.0 - t
                current_path.append(
                    (
                        u**3 * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t**3 * x3,
                        u**3 * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t**3 * y3,
                    )
                )
            x, y = x3, y3
        else:
            raise RuntimeError(f"Unsupported SVG path command: {command}")
    if current_path:
        paths.append(current_path)
    return paths


def render_symbolkit(path: Path) -> Image.Image:
    root = ET.parse(path).getroot()
    regular = _svg_node(root, "Regular-S")
    symbol_path = next((node for node in regular.iter() if node.tag.endswith("path")), None)
    if symbol_path is None or not symbol_path.attrib.get("d"):
        raise RuntimeError(f"Regular-S path missing in {path}")

    left = float(_svg_node(root, "left-margin-Regular-S").attrib["x1"])
    right = float(_svg_node(root, "right-margin-Regular-S").attrib["x1"])
    top = float(_svg_node(root, "Capline-S").attrib["y1"])
    bottom = float(_svg_node(root, "Baseline-S").attrib["y1"])
    width, height = SYMBOL_CANVAS
    scale = SYMBOL_OVERSAMPLE
    image = Image.new("L", (width * scale, height * scale), 255)
    draw = ImageDraw.Draw(image)
    for subpath in _flatten_path(symbol_path.attrib["d"]):
        points = [
            (
                round((x - left) * width * scale / (right - left)),
                round((y - top) * height * scale / (bottom - top)),
            )
            for x, y in subpath
        ]
        if len(points) >= 3:
            draw.polygon(points, fill=0)
    return image.resize((width, height), Image.Resampling.LANCZOS).convert("RGB")


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


def generate(input_path: Path, output_path: Path, mode_svg_dir: Path | None = None,
             preview_dir: Path | None = None) -> None:
    image = Image.open(input_path).convert("RGB")
    if image.size != (684, 1216):
        raise RuntimeError(f"Expected a 684x1216 render, got {image.size}")

    assets = []
    for enum_name, symbol, region, target, symbolkit_file in ICONS:
        if symbolkit_file is not None and mode_svg_dir is not None:
            source = render_symbolkit(mode_svg_dir / symbolkit_file)
            if preview_dir is not None:
                preview_dir.mkdir(parents=True, exist_ok=True)
                source.save(preview_dir / f"{enum_name}.png")
            bounds = ink_bounds(source, (0, 0, source.width, source.height))
            data, width, height, stride = pack_1bpp(source, bounds)
            anchor_x = source.width // 2 - bounds[0]
            anchor_y = source.height // 2 - bounds[1]
        else:
            if region is None or target is None:
                raise RuntimeError(f"{enum_name} requires --mode-svg-dir")
            bounds = ink_bounds(image, region)
            data, width, height, stride = pack_1bpp(image, bounds)
            anchor_x = target[0] - bounds[0]
            anchor_y = target[1] - bounds[1]
        assets.append((enum_name, symbol, data, width, height, stride, anchor_x, anchor_y))

    out = [
        "// Generated from Assets/Music.svg and SymbolKit playback-mode SVGs; do not hand-edit bitmap data.",
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
    parser.add_argument("--mode-svg-dir", type=Path,
                        help="Directory containing SymbolKit playback/control SVGs")
    parser.add_argument("--preview-dir", type=Path,
                        help="Optional output directory for rendered SymbolKit previews")
    args = parser.parse_args()
    generate(args.input, args.output, args.mode_svg_dir, args.preview_dir)


if __name__ == "__main__":
    main()
