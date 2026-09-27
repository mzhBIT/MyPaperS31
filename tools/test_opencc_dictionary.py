"""Small format/priority smoke test; run with `python -m unittest tools.test_opencc_dictionary`."""

import pathlib
import struct
import tempfile
import unittest
import zlib

from tools.build_opencc_dictionary import ENTRY, HEADER, MAGIC, SOURCES, build, load_entries


class DictionaryTest(unittest.TestCase):
    def test_phrase_precedes_character_and_binary_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "TSPhrases.txt").write_text("頭髮\t头发\n", encoding="utf-8")
            (root / "TSCharacters.txt").write_text("頭\t头\n髮\t发\n", encoding="utf-8")
            entries = load_entries(root, SOURCES["t2s"])
            self.assertEqual(entries["頭髮"], "头发")
            blob = build(entries, 2)
            magic, version, direction, count, maximum, records_size, data_size, checksum = HEADER.unpack_from(blob)
            self.assertEqual((magic, version, direction, count, maximum), (MAGIC, 1, 2, 3, 2))
            self.assertEqual(records_size, count * ENTRY.size)
            self.assertEqual(len(blob), HEADER.size + records_size + data_size)
            self.assertEqual(zlib.crc32(blob[HEADER.size:]), checksum)
            data = blob[HEADER.size + records_size:]
            decoded = {}
            for i in range(count):
                key_off, value_off, key_len, value_len = ENTRY.unpack_from(blob, HEADER.size + i * ENTRY.size)
                decoded[data[key_off:key_off + key_len].decode()] = data[value_off:value_off + value_len].decode()
            self.assertEqual(decoded, entries)


if __name__ == "__main__":
    unittest.main()
