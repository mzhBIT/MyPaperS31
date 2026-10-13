"""Extract the original generated Lucide folder/file masks, without re-rasterizing.

Usage: python tools/import_pico_file_icons.py PATH_TO_READ_PICO_READER
Source artwork: Lucide (ISC). Original masks: 2026 mindreset (Apache-2.0).
"""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
args = parser.parse_args()
source = (args.source / 'main/assets/ui_icons.h').read_text(encoding='utf-8-sig')
output = [
    '// Original read-pico-reader 72px alpha masks (2026 mindreset, Apache-2.0).',
    '// Lucide folder/file artwork, ISC; see docs/licenses/pico-shelf-icons.txt.',
    '#pragma once', '#include <cstdint>', 'namespace pico_file_icons {',
]
for name in ('folder', 'file'):
    match = re.search(r'\{  // ' + name + r'\n(.*?)\n    \}', source, re.S)
    if not match or len(re.findall(r'0x[0-9a-fA-F]{2}', match[1])) != 2592:
        raise SystemExit('Missing or invalid 72px mask: ' + name)
    output.extend(['inline constexpr uint8_t ' + name + '[] = {', match[1], '};'])
output.append('}')
destination = Path(__file__).resolve().parents[1] / 'main/ui/pico_file_icons.h'
destination.write_text('\n'.join(output) + '\n', encoding='utf-8')
print('Imported folder/file: 2592 bytes each')
