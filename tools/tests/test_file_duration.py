"""Exercise the native duration probe with container headers and malformed input.

Usage: python tools/tests/test_file_duration.py path/to/file_duration_probe.exe
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

be = lambda n: struct.pack('>I', n)
le = lambda n: struct.pack('<I', n)
def atom(kind, data, extended=False):
    return (be(1) + kind + struct.pack('>Q', len(data) + 16) if extended
            else be(len(data) + 8) + kind) + data
def track(kind, rate, duration, version=0):
    mdhd = ((b'\x01\0\0\0' + bytes(16) + be(rate) + struct.pack('>Q', duration))
            if version else bytes(12) + be(rate) + be(duration))
    return atom(b'trak', atom(b'mdia', atom(b'mdhd', mdhd) +
                             atom(b'hdlr', bytes(8) + kind)))
def page(serial, flags, granule, data):
    lengths = bytes([255]) * (len(data) // 255) + bytes([len(data) % 255])
    return (b'OggS\0' + bytes([flags]) + struct.pack('<Q', granule) + le(serial) +
            le(0) + le(0) + bytes([len(lengths)]) + lengths + data)
def adts(index, blocks=0):
    n = 107
    return bytes([255, 241, 64 | index << 2, 128 | n >> 11,
                  n >> 3 & 255, (n & 7) << 5 | 31, 252 | blocks]) + bytes(n - 7)

def main():
    probe = str(pathlib.Path(sys.argv[1]).resolve())
    tests = []
    def add(name, content, duration): tests.append((name, content, duration))
    info = bytearray(34)
    info[10:18] = struct.pack('>Q', 44100 << 44 | (2 - 1) << 41 | (16 - 1) << 36 | 44100 * 302)
    flac = b'fLaC\x80\0\0\x22' + info
    add('exact.flac', flac, 302000)
    add('tagged.flac', b'ID3\x04\0\0\0\0\0\x04test' + flac, 302000)
    fmt = struct.pack('<HHIIHH', 1, 2, 16000, 64000, 4, 16)
    chunks = b'JUNK' + le(3) + b'abc\0' + b'fmt ' + le(16) + fmt + b'data' + le(64000) + bytes(64000)
    add('pcm.wav', b'RIFF' + le(len(chunks) + 4) + b'WAVE' + chunks, 1000)
    movie = track(b'vide', 1000, 999000) + track(b'soun', 48000, 48000 * 302)
    add('audio.m4a', atom(b'ftyp', b'M4A ' + bytes(4)) + atom(b'mdat', bytes(150000)) + atom(b'moov', movie), 302000)
    add('long.mp4', atom(b'moov', track(b'soun', 48000, 48000 * 302, 1), True), 302000)
    opus = b'OpusHead\x01\x02' + struct.pack('<H', 312) + le(44100) + bytes(3)
    stream = page(7, 2, 0, opus) + page(7, 4, 48000 * 3 + 312, b'audio')
    add('exact.opus', stream, 3000)
    add('chain.ogg', stream + page(8, 2, 0, opus) + page(8, 4, 48000 + 312, b'audio'), 4000)
    vorbis = b'\x01vorbis' + le(0) + b'\x02' + le(22050) + bytes(14)
    add('vorbis.ogg', page(9, 2, 0, vorbis) + page(9, 4, 44100, b'audio'), 2000)
    add('vbr.aac', adts(4) * 50, round(50 * 1024 * 1000 / 44100))
    add('multiblock.aac', adts(8, 1) * 10, 1280)
    frame = bytes([255, 251, 144, 0]) + bytes(413)
    add('scan.mp3', frame * 100, round(100 * 1152 * 1000 / 44100))
    xing = bytearray(frame)
    xing[36:48] = b'Xing' + be(1) + be(100)
    add('tag.mp3', xing + frame * 99, round(100 * 1152 * 1000 / 44100))
    for ext in ('flac', 'wav', 'm4a', 'aac', 'ogg', 'opus', 'mp3'):
        add('invalid.' + ext, b'garbage' * 9, 0)
    add('truncated.aac', adts(4)[:-1], 0)
    add('truncated.ogg', stream[:-1], 0)
    add('huge-atom.m4a', be(0xffffffff) + b'moov', 0)
    with tempfile.TemporaryDirectory() as folder:
        for name, content, expected in tests:
            path = pathlib.Path(folder, name)
            path.write_bytes(content)
            output = subprocess.check_output([probe, str(path)], text=True)
            ok, duration, restored = map(int, output.split())
            assert restored and bool(ok) == bool(expected) and abs(duration - expected) <= 1, (name, output, expected)
            output = subprocess.check_output([probe, str(path), 'cancel'], text=True)
            assert output.strip() == '0 0 1', (name, 'cancel', output)
    print(f'PASS: {len(tests)} metadata/malformed cases plus cancellation/cursor restoration')

if __name__ == '__main__': main()
