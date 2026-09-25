"""Rasterize the project's Feather phone path for the 150px home icon table."""

from pathlib import Path

from PIL import Image, ImageDraw


def main() -> None:
    source = Path("Assets/feather/phone.svg").read_text(encoding="utf-8")
    if 'class="feather feather-phone"' not in source:
        raise RuntimeError("unexpected phone SVG")
    # Vertices follow the handset outline in phone.svg. Render at 4x before
    # reducing to the 1-bit home-icon resolution to preserve rounded corners.
    vertices = [
        (22, 16.92), (22, 19.92), (19.82, 21.92), (16.2, 21.45),
        (11.19, 18.85), (7.8, 16.15), (5.19, 12.85), (2.55, 7.8),
        (2.12, 4.18), (4.11, 2), (7.11, 2), (9.11, 3.72),
        (9.81, 6.53), (9.36, 8.64), (8.09, 9.91),
        (10.7, 13.3), (14.09, 15.91), (15.36, 14.64),
        (17.47, 14.19), (20.28, 14.89), (22, 16.92),
    ]
    factor = 25  # 24 SVG units * 25 = 600 px = 4x output.
    image = Image.new("L", (600, 600), 255)
    draw = ImageDraw.Draw(image)
    points = [(round(x * factor), round(y * factor)) for x, y in vertices]
    draw.line(points, fill=0, width=2 * factor, joint="curve")
    for x, y in points:
        draw.ellipse((x - factor, y - factor, x + factor, y + factor), fill=0)
    image.resize((150, 150), Image.Resampling.LANCZOS).save("tmp/phone-icon-150.png")


if __name__ == "__main__":
    main()
