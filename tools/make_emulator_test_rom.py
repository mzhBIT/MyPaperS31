"""Generate an original, logo-free DMG diagnostic ROM (no commercial game data)."""
from pathlib import Path
import argparse


def make_rom(audio=False):
    rom = bytearray(32768)
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x134:0x13C] = b'S31 DEMO'
    if audio:
        rom[0x134:0x142] = b'S31 AUDIO DEMO'
    rom[0x147] = 3  # MBC1 + RAM + battery, to exercise persistent saves.
    rom[0x149] = 2  # 8 KiB RAM.
    code = bytearray()
    labels = {}
    patches = []

    def emit(*values):
        code.extend(values)

    def label(name):
        labels[name] = len(code)

    def jump(op, name):
        emit(op, 0)
        patches.append((len(code) - 1, name))

    emit(0xF3, 0x31, 0xFE, 0xFF, 0xAF, 0xE0, 0x40)  # DI, stack, LCD off.
    emit(0x21, 0x00, 0x80, 0x06, 8)
    label('tiles')
    emit(0x36, 0xAA, 0x23, 0x36, 0xCC, 0x23, 0x05)
    jump(0x20, 'tiles')
    emit(0x3E, 0x0A, 0xEA, 0, 0)  # Enable cartridge RAM.
    emit(0xAF, 0xEA, 0, 0xC0, 0xE0, 0x42, 0xE0, 0x43)
    emit(0x3E, 0xE4, 0xE0, 0x47, 0x3E, 0x91, 0xE0, 0x40)
    if audio:
        # Channel 1, 50% square wave, about 440 Hz, routed to both speakers.
        for register, value in [(0x26,0x80),(0x24,0x77),(0x25,0x11),
                                (0x10,0),(0x11,0x80),(0x12,0xf0),(0x13,0xd6),(0x14,0x86)]:
            emit(0x3e,value,0xe0,register)
    label('vblank')
    emit(0xF0, 0x44, 0xFE, 144)
    jump(0x20, 'vblank')
    emit(0xFA, 0, 0xC0, 0x3C, 0xEA, 0, 0xC0, 0xEA, 0, 0xA0, 0xE0, 0x43)
    emit(0x3E, 0x10, 0xE0, 0, 0xF0, 0, 0xE6, 1, 0x3E, 0xE4)
    jump(0x20, 'palette')
    emit(0x3E, 0x1B)  # Holding A reverses the four-shade palette.
    label('palette')
    emit(0xE0, 0x47)
    if audio:
        emit(0xfe,0x1b)
        jump(0x20,'low_note')
        emit(0x3e,0x6b,0xe0,0x13,0x3e,7,0xe0,0x14)
        jump(0x18,'note_done')
        label('low_note')
        emit(0x3e,0xd6,0xe0,0x13,0x3e,6,0xe0,0x14)
        label('note_done')
    label('visible')
    emit(0xF0, 0x44, 0xFE, 144)
    jump(0x30, 'visible')
    jump(0x18, 'vblank')
    for position, target in patches:
        displacement = labels[target] - position - 1
        assert -128 <= displacement <= 127
        code[position] = displacement & 255
    rom[0x150:0x150 + len(code)] = code
    checksum = 0
    for value in rom[0x134:0x14D]:
        checksum = (checksum - value - 1) & 255
    rom[0x14D] = checksum
    total = sum(rom[:0x14E]) + sum(rom[0x150:])
    rom[0x14E:0x150] = (total & 65535).to_bytes(2, 'big')
    return rom


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('tmp/emulator/S31-Demo.gb'))
    parser.add_argument('--audio',action='store_true',help='emit a 440 Hz test tone; A changes it to about 880 Hz')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(make_rom(audio=args.audio))
    print(f'Original DMG diagnostic: {args.output} ({args.output.stat().st_size} bytes)')
