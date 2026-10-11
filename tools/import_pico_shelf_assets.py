"""Import only the Lucide masks used by the Pico bookshelf (build is standalone)."""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('reference', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (args.reference / 'main/assets/ui_icons.h').read_text(encoding='utf-8')
names = ['house', 'library-big', 'folder-open', 'sliders-horizontal',
         'chevron-left', 'chevron-right', 'wifi']
out = ['// Generated from read-pico-reader UI icon masks; Lucide icons, ISC license.',
       '// See docs/licenses/pico-shelf-icons.txt. Do not edit by hand.',
       '#pragma once', '#include <cstdint>', 'namespace pico_shelf_icons {']
for name in names:
    match = re.search(r'\{\s*// ' + re.escape(name) + r'\s*\n(.*?)\n\s*\}', source, re.S)
    if not match:
        raise ValueError(name)
    values = re.findall(r'0x[0-9a-fA-F]+', match[1])
    assert len(values) == 72 * 72 // 2, (name, len(values))
    out.append('inline constexpr uint8_t ' + name.replace('-', '_') + '[] = {')
    out.extend('    ' + ', '.join(values[i:i+24]) + ',' for i in range(0, len(values), 24))
    out.append('};')
out.append('}')
(root / 'main/ui/pico_shelf_icons.h').write_text('\n'.join(out) + '\n', encoding='utf-8')
licenses = root / 'docs/licenses'
licenses.mkdir(exist_ok=True)
(licenses / 'pico-shelf-icons.txt').write_text(
    (args.reference / 'tools/icons/lucide/LICENSE').read_text(encoding='utf-8'), encoding='utf-8')
(licenses / 'pico-reader-Apache-2.0.txt').write_text(
    (args.reference / 'LICENSE').read_text(encoding='utf-8'), encoding='utf-8')
print('Imported', len(names), '72x72 coverage masks')
