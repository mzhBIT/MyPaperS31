"""Host tests: python tools/test_offline_map.py"""
import tempfile
from pathlib import Path
import unittest

from PIL import Image
import build_offline_map as m


class OfflineMapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def tile(self, z=0, x=0, y=0):
        p = self.root / str(z) / str(x) / f"{y}.png"
        p.parent.mkdir(parents=True, exist_ok=True)
        image = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        image.putpixel((0, 0), (0, 0, 0, 255))
        image.putpixel((1, 0), (255, 255, 255, 255))
        image.putpixel((2, 0), (0, 0, 0, 128))
        image.save(p)
        return p

    def test_alpha_quantization_and_nibble_order(self):
        payload = m.gray4_tile(self.tile())
        self.assertEqual(len(payload), 32768)
        self.assertEqual(payload[0], 0x0f)
        self.assertEqual(payload[1], 0x7f)
        self.assertEqual(payload[-1], 0xff)

    def test_roundtrip_and_corruption(self):
        self.tile()
        output = self.root / "test.mpm"
        m.build_pack(m.find_tiles(self.root, [0]), output)
        fields = m.verify_pack(output)
        self.assertEqual(fields[4], 1)
        self.assertEqual(output.stat().st_size, 96 + 32 + 32768)
        with output.open("r+b") as f:
            f.seek(-1, 2)
            f.write(b"\0")
        with self.assertRaisesRegex(ValueError, "CRC mismatch"):
            m.verify_pack(output)

    def test_v2_uniform_and_mixed(self):
        tiles = []
        for x in range(4):
            path = self.tile(2, x, 0)
            if x < 3:
                Image.new('L', (256,256), x*119).save(path)
            tiles.append(((2,x,0),path))
        output = self.root/'v2.mpm'
        m.build_pack(tiles, output, version=2, workers=4)
        self.assertEqual(m.verify_pack(output)[1], 2)
        self.assertEqual(output.stat().st_size, 96+4*32+32768)
        with output.open('rb') as stream:
            stream.seek(96)
            entries = [m.ENTRY.unpack(stream.read(32)) for _ in tiles]
        self.assertEqual([e[1] for e in entries], [1,1,1,0])
        self.assertEqual([e[2] for e in entries], [0,7,14,0])
        # Entirely uniform files contain only the header and index.
        blank = self.root/'blank.mpm'
        m.build_pack(tiles[:1], blank, version=2)
        self.assertEqual(blank.stat().st_size,128)
        m.verify_pack(blank)

    def test_vector_quantization_matches_fallback(self):
        path = self.tile()
        im = Image.new('L',(256,256))
        im.putdata([i%256 for i in range(65536)])
        im.save(path)
        fast = m.gray4_tile(path)
        numpy = m.np
        try:
            m.np = None
            self.assertEqual(fast,m.gray4_tile(path))
        finally:
            m.np = numpy

    def test_missing_and_truncated_tiles(self):
        path = self.tile(1, 0, 0)
        with self.assertRaisesRegex(ValueError, "missing"):
            m.find_tiles(self.root, [1], [-180, -80, 180, 80])
        path.write_bytes(b"not a png")
        with self.assertRaises(OSError):
            m.build_pack([((1, 0, 0), path)], self.root / "bad.mpm")
        self.assertFalse((self.root / "bad.mpm").exists())
        self.assertFalse((self.root / "bad.mpm.tmp").exists())

    def test_order_preview_and_no_overwrite(self):
        for x in range(4):
            for y in range(4):
                self.tile(2, x, y)
        output = self.root / "test.mpm"
        tiles = m.find_tiles(self.root, [2])
        self.assertEqual([k for k, _ in tiles], sorted(k for k, _ in tiles))
        m.build_pack(tiles, output)
        with self.assertRaisesRegex(ValueError, "already exists"):
            m.build_pack(tiles, output)
        png = self.root / "preview.png"
        m.preview_pack(output, png, 2, 0, 0, 256, 256)
        with Image.open(png) as im:
            self.assertEqual(im.size, (256, 256))


if __name__ == "__main__":
    unittest.main()
