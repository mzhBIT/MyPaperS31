"""Rasterize Assets/feather/heart.svg to a 32px monochrome comment icon."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw
from generate_home_status_icons import arc_points

ROOT = Path(__file__).resolve().parents[1]
SIZE = 32

def main():
    svg = ET.parse(ROOT / "Assets/feather/heart.svg").getroot()
    path = next(node for node in svg if node.tag.endswith("path"))
    tokens = re.findall(r"[MLlaz]|[-+]?(?:\d+(?:\.\d*)?|\.\d+)", path.attrib["d"])
    points = []
    x = y = 0.0
    index = 0
    command = ""
    while index < len(tokens):
        if tokens[index].isalpha():
            command = tokens[index]
            index += 1
        if command == "z":
            points.append(points[0])
            break
        count = 7 if command == "a" else 2
        values = [float(value) for value in tokens[index:index + count]]
        index += count
        if len(values) != count:
            raise ValueError("Malformed heart path")
        if command == "a":
            rx, ry, rotation, large, sweep, dx, dy = values
            points.extend(arc_points(x, y, rx, ry, rotation, large, sweep, x + dx, y + dy)[1:])
            x, y = x + dx, y + dy
        elif command in ("M", "L", "l"):
            dx, dy = values
            x, y = (x + dx, y + dy) if command == "l" else (dx, dy)
            points.append((x, y))
            if command == "M":
                command = "L"
        else:
            raise ValueError(f"Unsupported path command {command}")
    scale = SIZE * 8 / 24
    image = Image.new("L", (SIZE * 8, SIZE * 8), 255)
    draw = ImageDraw.Draw(image)
    stroke = float(svg.get("stroke-width", "2")) * scale
    pixels = [(round(px * scale), round(py * scale)) for px, py in points]
    draw.line(pixels, fill=0, width=round(stroke), joint="curve")
    # Rounded joins/endcaps, matching Feather's stroke-linejoin/linecap.
    for px, py in pixels:
        r = stroke / 2
        draw.ellipse((px-r, py-r, px+r, py+r), fill=0)
    image = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    image.save(ROOT / "Assets/weread-heart.png")
    data = bytearray(SIZE * 4)
    for y in range(SIZE):
        for x in range(SIZE):
            if image.getpixel((x, y)) < 192:
                data[y * 4 + x // 8] |= 0x80 >> (x % 8)
    assert any(data) and not all(value == 255 for value in data)
    lines = ["// Generated from Assets/feather/heart.svg; do not edit.", "#pragma once",
             "#include <cstdint>", "namespace weread_icons {",
             "constexpr int heart_size = 32;", "constexpr uint8_t heart[128] = {"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in data[offset:offset+16]) + ",")
    lines.extend(["};", "} // namespace weread_icons"])
    (ROOT / "main/ui/weread_icons.h").write_text("\n".join(lines) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
