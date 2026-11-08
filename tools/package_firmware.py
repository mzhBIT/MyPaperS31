"""Validate S31 build artifacts and create a fully offline browser-flash package.

Run with the ESP-IDF Python environment (esptool is needed for image checks).
No device is opened, reset or flashed by this packaging command.
"""
from __future__ import annotations
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
from datetime import datetime, timezone, timedelta
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / 'tools/web-flasher'
CONTRACT = {
    'nvs': (1, 2, 0x9000, 0x6000), 'phy_init': (1, 1, 0xf000, 0x1000),
    'factory': (0, 0, 0x10000, 0x800000), 'emulator': (0, 16, 0x810000, 0x400000),
    'otadata': (1, 0, 0xc10000, 0x2000),
}
SLOTS = {'bootloader': (0x2000, 0x6000), 'partitions': (0x8000, 0x1000),
         'system': (0x10000, 0x800000), 'emulator': (0x810000, 0x400000), 'otadata': (0xc10000, 0x2000)}

def check_table(data: bytes):
    if len(data) < 0xc00:
        raise ValueError('Partition table is truncated')
    actual = {}
    for pos in range(0, 0xc00, 32):
        magic = struct.unpack_from('<H', data, pos)[0]
        if magic == 0xebeb:
            if data[pos+16:pos+32] != hashlib.md5(data[:pos]).digest():
                raise ValueError('Partition table MD5 mismatch')
            if actual != CONTRACT:
                raise ValueError(f'Unexpected S31 dual-firmware partition layout: {actual}')
            return actual
        if magic != 0x50aa:
            raise ValueError('Partition table is invalid or lacks MD5')
        _, kind, subtype, offset, size, raw_name, flags = struct.unpack_from('<HBBII16sI', data, pos)
        name = raw_name.split(b'\0', 1)[0].decode('ascii')
        if not name or name in actual or flags:
            raise ValueError('Duplicate/unnamed/encrypted partition')
        actual[name] = (kind, subtype, offset, size)
    raise ValueError('Partition table lacks MD5')

def check_image(path: Path, expected_project: str | None):
    from esptool.bin_image import LoadFirmwareImage
    image = LoadFirmwareImage('esp32s31', str(path))
    image.verify()
    if image.chip_id != 32:
        raise ValueError(f'{path.name}: not an ESP32-S31 image')
    result = {}
    if expected_project:
        desc = image.segments[0].data
        if len(desc) < 144 or struct.unpack_from('<I', desc)[0] != 0xabcd5432:
            raise ValueError(f'{path.name}: missing application descriptor')
        text = lambda start, end: desc[start:end].split(b'\0', 1)[0].decode('utf-8')
        result = dict(project=text(48, 80), version=text(16, 48), time=text(80, 96), date=text(96, 112), sdk=text(112, 144))
        if result['project'] != expected_project:
            raise ValueError(f'{path.name}: expected {expected_project}, got {result["project"]}')
    return result

def browser_library():
    source = (WEB / 'vendor/esptool-js-0.7.1.js').read_text(encoding='utf-8')
    if hashlib.sha256(source.encode()).hexdigest() != 'd6a4b0b7c9b747a7370cfa976164a962f56332d5faa7c3b45cdb95a2a8b1d456':
        raise ValueError('Unexpected esptool-js distribution; review vendored update before packaging')
    # The official Rollup bundle contains its targets, S31 stub and dependencies.
    # Convert its final ESM export statement into a classic-script namespace so
    # a file:// page needs neither import maps nor cross-origin module requests.
    match = re.search(r'export\s*\{([^{}]+)\};?\s*$', source)
    if not match or re.search(r'\b(?:import\s*\(|fetch\s*\(|import\.meta)', source):
        raise ValueError('The vendor bundle is not self-contained')
    fields = []
    for item in match[1].split(','):
        parts = re.split(r'\s+as\s+', item.strip())
        if len(parts) == 1: parts.append(parts[0])
        if not all(re.fullmatch(r'\w+', word) for word in parts): raise ValueError('Invalid vendor export')
        fields.append(f'{parts[1]}:{parts[0]}')
    return '(function(){\n' + source[:match.start()] + '\nglobalThis.MyPaperEspTool={' + ','.join(fields) + '};\n})();'

def make_package(system_build: Path, emulator_build: Path, output: Path, archive: Path | None):
    sys_manifest = json.loads((system_build/'flasher_args.json').read_text())
    emu_manifest = json.loads((emulator_build/'flasher_args.json').read_text())
    for item in (sys_manifest, emu_manifest):
        if item['extra_esptool_args']['chip'] != 'esp32s31': raise ValueError('Wrong build target')
    if sys_manifest['flash_settings'] != dict(flash_mode='dio', flash_size='16MB', flash_freq='80m'):
        raise ValueError('Unexpected system Flash parameters')
    paths = {
        'bootloader': system_build/'bootloader/bootloader.bin',
        'partitions': system_build/'partition_table/partition-table.bin',
        'system': system_build/'MyPaperS31.bin', 'emulator': emulator_build/'MyPaperEmulatorS31.bin',
        'otadata': system_build/'ota_data_initial.bin',
    }
    for key, slot in SLOTS.items():
        expected = paths[key].relative_to(emulator_build if key == 'emulator' else system_build).as_posix()
        recorded = (emu_manifest if key == 'emulator' else sys_manifest)['flash_files']
        if recorded.get(hex(slot[0])) != expected: raise ValueError(f'Build flash manifest differs for {key}')
    data = {key: path.read_bytes() for key, path in paths.items()}
    check_table(data['partitions'])
    if (emulator_build/'partition_table/partition-table.bin').read_bytes() != data['partitions']:
        raise ValueError('System and emulator use different partition tables')
    if data['otadata'] != b'\xff'*0x2000: raise ValueError('Initial OTA data must select factory on erased metadata')
    app = check_image(paths['system'], 'MyPaperS31')
    emulator = check_image(paths['emulator'], 'MyPaperEmulatorS31')
    check_image(paths['bootloader'], None)
    now = datetime.now(timezone(timedelta(hours=8)))
    version = now.strftime('%Y.%m.%d') + '-' + app['version']
    manifest = dict(schema=1, board='MyPaperS31', chip='ESP32-S31', chipId=32, flashBytes=0x1000000,
                    version=version, packagedAt=now.isoformat(timespec='seconds'),
                    system=app, emulator=emulator, files=[])
    output_files = {}
    for key, raw in data.items():
        address, budget = SLOTS[key]
        if not raw or len(raw) > budget or (len(raw)+4095)//4096*4096 > budget or len(raw)%4:
            raise ValueError(f'{key} image does not fit its write/erase budget')
        filename = 'firmware/' + paths[key].name
        output_files[filename] = raw
        manifest['files'].append(dict(id=key, path=filename, address=address, size=len(raw),
                                     sha256=hashlib.sha256(raw).hexdigest(), md5=hashlib.md5(raw).hexdigest()))
    output_files['manifest.json'] = (json.dumps(manifest, ensure_ascii=False, indent=2)+'\n').encode()
    output_files['partitions.csv'] = (ROOT/'configs/mypapers31/partitions.csv').read_bytes()
    payload = dict(manifest=manifest, images={key:base64.b64encode(raw).decode() for key,raw in data.items()})
    html = (WEB/'index.template.html').read_text(encoding='utf-8')
    replacements = {'STYLES':(WEB/'styles.css').read_text(encoding='utf-8'), 'PACKAGE':json.dumps(payload, ensure_ascii=False, separators=(',', ':')),
                    'VENDOR':browser_library(), 'MD5':(WEB/'vendor/js-md5-0.8.3.js').read_text(encoding='utf-8'),
                    'CORE':(WEB/'core.js').read_text(encoding='utf-8'), 'APP':(WEB/'app.js').read_text(encoding='utf-8')}
    for key, value in replacements.items():
        marker = f'/*__{key}__*/'
        if html.count(marker) != 1: raise ValueError('Missing or duplicate template marker: '+key)
        html = html.replace(marker, re.sub(r'</script', r'<\/script', value, flags=re.I))
    output_files['刷机工具.html'] = html.encode('utf-8')
    instructions = (WEB/'USER_GUIDE.md').read_text(encoding='utf-8').replace('{{VERSION}}', version)
    output_files['使用说明.md'] = instructions.encode('utf-8')
    for name in ('esptool-js-LICENSE.txt', 'js-md5-LICENSE.txt', 'pako-LICENSE.txt', 'pako-zlib-LICENSE.txt', 'atob-lite-LICENSE.txt', 'THIRD_PARTY.md'):
        output_files['licenses/'+name] = (WEB/'vendor'/name).read_bytes()
    checksums = ''.join(f'{hashlib.sha256(raw).hexdigest()}  {name}\n' for name,raw in output_files.items())
    output_files['SHA256SUMS.txt'] = checksums.encode('utf-8')
    output.mkdir(parents=True, exist_ok=True)
    for name, raw in output_files.items():
        path = output/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
    if archive:
        archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
            for name, raw in output_files.items(): zipped.writestr('MyPaperS31/'+name, raw)
    print(json.dumps(dict(output=str(output), archive=str(archive) if archive else None,
                         version=version, files=len(output_files), firmware_bytes=sum(map(len,data.values())),
                         html_bytes=len(output_files['刷机工具.html'])), ensure_ascii=False))
    return manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system-build', type=Path, default=ROOT/'build-s31')
    parser.add_argument('--emulator-build', type=Path, default=ROOT/'build-s31-emulator')
    parser.add_argument('--output', type=Path, default=ROOT/'firmware-release/MyPaperS31')
    parser.add_argument('--zip', type=Path, dest='archive', default=ROOT/'firmware-release/MyPaperS31-offline.zip')
    args = parser.parse_args()
    make_package(args.system_build.resolve(), args.emulator_build.resolve(), args.output.resolve(), args.archive.resolve())

if __name__ == '__main__': main()
