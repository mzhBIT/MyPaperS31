"""Extract the Regular-S glyph from the supplied SF Symbols export sheet."""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
ns = "{http://www.w3.org/2000/svg}"
source = ET.parse(root / "Assets/chart-candlestick.svg")
glyph = next(g for g in source.iter(ns + "g") if g.get("id") == "Regular-S")
ET.register_namespace("", "http://www.w3.org/2000/svg")
svg = ET.Element(ns + "svg", {"viewBox": "1410 621 80 80", "width": "150", "height": "150"})
ET.SubElement(svg, ns + "rect", {"x": "1410", "y": "621", "width": "80", "height": "80", "fill": "white"})
for path in glyph.iter(ns + "path"):
    ET.SubElement(svg, ns + "path", {"d": path.get("d"), "fill": "black"})
ET.ElementTree(svg).write(root / "Assets/stocks-home.svg", encoding="utf-8", xml_declaration=True)
