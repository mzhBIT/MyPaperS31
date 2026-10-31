"""Reject a S31 emulator image that links a reader display or network backend."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--elf', type=Path, required=True)
parser.add_argument('--nm', type=Path, required=True)
args = parser.parse_args()
result = subprocess.run([str(args.nm), '-C', '--defined-only', str(args.elf)],
                        check=True, capture_output=True, text=True, encoding='utf-8')
symbols = result.stdout
for forbidden in ('Panel_EPD', 'Bus_EPD', 'epd2d_continuous', 'M5GFX::init',
                  'M5Unified::begin', 'esp_wifi_init', 'usb_host_install'):
    if forbidden in symbols:
        raise SystemExit(f'Emulator isolation failed: unexpected symbol {forbidden}')
for required in ('emulator::PaperDisplay::run()', 'emu_gb_frame',
                 'emulator::GameAudioOutput::run()', 'minigb_apu_audio_render'):
    if required not in symbols:
        raise SystemExit(f'Emulator isolation failed: missing {required}')
print('S31 emulator isolation PASS: independent MSG scan + GB/APU + speaker; no reader EPD/Wi-Fi/USB Host')
