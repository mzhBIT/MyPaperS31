"""Generate ZIP transport regression fixtures in the workspace tmp directory."""
import pathlib
import struct
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2] / 'tmp' / 'comic-tests'
ROOT.mkdir(parents=True, exist_ok=True)
names = ['book/10.bmp', 'book/02.png', 'book/2.JPEG', 'book/1.jpg',
         'book/3# & ?.png', 'part10/01.png', 'part2/01.jpg',
         '__MACOSX/._0.jpg', '.hidden/01.png', 'readme.txt']
with zipfile.ZipFile(ROOT / 'normal.cbz', 'w') as archive:
    for index, name in enumerate(names):
        archive.writestr(name, name.encode() * 100,
                         compress_type=zipfile.ZIP_STORED if index % 2 else zipfile.ZIP_DEFLATED)
with zipfile.ZipFile(ROOT / 'empty.zip', 'w') as archive:
    archive.writestr('readme.txt', 'No images')
with zipfile.ZipFile(ROOT / 'encrypted.zip', 'w') as archive:
    archive.writestr('1.png', 'encrypted fixture')
blob = bytearray((ROOT / 'encrypted.zip').read_bytes())
struct.pack_into('<H', blob, 6, 1)
central = blob.index(b'PK\x01\x02')
struct.pack_into('<H', blob, central + 8, 1)
(ROOT / 'encrypted.zip').write_bytes(blob)
with zipfile.ZipFile(ROOT / 'unsupported.zip', 'w') as archive:
    archive.writestr('1.jpg', 'unsupported fixture')
blob = bytearray((ROOT / 'unsupported.zip').read_bytes())
struct.pack_into('<H', blob, 8, 99)
central = blob.index(b'PK\x01\x02')
struct.pack_into('<H', blob, central + 10, 99)
(ROOT / 'unsupported.zip').write_bytes(blob)
blob = bytearray((ROOT / 'normal.cbz').read_bytes())
eocd = blob.rindex(b'PK\x05\x06')
struct.pack_into('<I', blob, eocd + 16, 0xffffffff)
(ROOT / 'zip64.zip').write_bytes(blob)
with zipfile.ZipFile(ROOT / 'oversize.zip', 'w') as archive:
    archive.writestr('1.jpg', 'size fixture')
blob = bytearray((ROOT / 'oversize.zip').read_bytes())
central = blob.index(b'PK\x01\x02')
struct.pack_into('<I', blob, central + 24, 12 * 1024 * 1024 + 1)
(ROOT / 'oversize.zip').write_bytes(blob)
with zipfile.ZipFile(ROOT / 'numbers.zip', 'w') as archive:
    for name in ['10000000000000000000.jpg', '00010.jpg', '10.jpg', '9.jpg', '9999999999999999999.jpg']:
        archive.writestr(name, name)
print(f'Comic ZIP fixtures generated: {ROOT}')
