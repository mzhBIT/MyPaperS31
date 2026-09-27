"""Remove the baked edge frame from the original home icon bitmaps.

Run once when regenerating the first nine icons. The home UI draws a shared
frame, so the 150x150 bitmaps should contain only their icon artwork.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/ui/home_icons_1bpp.cpp"
WIDTH = HEIGHT = 150
STRIDE = 19
EDGE = 7
ORIGINAL_ICONS = (
    "clock", "transfer", "images", "files", "book", "news",
    "music", "radio", "setting",
)


def strip_frame(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(const uint8_t icon_{name}_data\[HOME_ICON_1BPP_DATA_SIZE\] = \{{)(.*?)(\n\}};)",
        re.DOTALL,
    )
    match = pattern.search(source)
    if match is None:
        raise ValueError(f"icon not found: {name}")
    body = match.group(2)
    tokens = list(re.finditer(r"0x[0-9A-Fa-f]{2}", body))
    if len(tokens) != STRIDE * HEIGHT:
        raise ValueError(f"{name}: expected {STRIDE * HEIGHT} bytes, got {len(tokens)}")
    data = bytearray(int(token.group(), 16) for token in tokens)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if x < EDGE or x >= WIDTH - EDGE or y < EDGE or y >= HEIGHT - EDGE:
                data[y * STRIDE + x // 8] &= ~(0x80 >> (x & 7))
    parts = []
    cursor = 0
    for index, token in enumerate(tokens):
        parts.append(body[cursor:token.start()])
        parts.append(f"0x{data[index]:02X}")
        cursor = token.end()
    parts.append(body[cursor:])
    return source[:match.start(2)] + "".join(parts) + source[match.end(2):]


def main() -> None:
    original = SOURCE.read_text(encoding="utf-8")
    updated = original
    for name in ORIGINAL_ICONS:
        updated = strip_frame(updated, name)
    if updated != original:
        with SOURCE.open("w", encoding="utf-8", newline="\n") as output:
            output.write(updated)


if __name__ == "__main__":
    main()
