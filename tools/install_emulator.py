"""Install only the S31 emulator slot, after checking the device partition table."""
from pathlib import Path
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from esptool.util import FatalError

OFFSET=0x810000
SIZE=0x400000
CONTRACT={
    'nvs':(1,2,0x9000,0x6000),
    'phy_init':(1,1,0xf000,0x1000),
    'factory':(0,0,0x10000,0x800000),
    'emulator':(0,0x10,OFFSET,SIZE),
    'otadata':(1,0,0xc10000,0x2000),
}

def check_table(table):
    actual={entry.name:(entry.type,entry.subtype,entry.offset,entry.size) for entry in table}
    for name,expected in CONTRACT.items():
        if actual.get(name)!=expected:
            raise ValueError(f'Device partition {name} does not match dual-firmware layout. Flash the updated SYSTEM firmware/table first.')

def check_image(path):
    from esptool.bin_image import LoadFirmwareImage
    if path.stat().st_size>SIZE:raise ValueError('Emulator exceeds its 4 MiB slot.')
    image=LoadFirmwareImage('esp32s31',str(path))
    from esptool.targets import CHIP_DEFS
    if image.chip_id!=CHIP_DEFS['esp32s31'].IMAGE_CHIP_ID:raise ValueError('Wrong chip image.')
    desc=image.segments[0].data
    if len(desc)<80 or struct.unpack_from('<I',desc)[0]!=0xabcd5432:
        raise ValueError('Missing ESP application descriptor.')
    if desc[48:80].split(b'\0',1)[0]!=b'MyPaperEmulatorS31':raise ValueError('This is not the emulator firmware.')
    image.verify()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--port',required=True)
    parser.add_argument('--baud',type=int,default=921600)
    args=parser.parse_args()
    check_image(args.image)
    sdk=Path(os.environ['IDF_PATH'])/'components/partition_table'
    sys.path.insert(0,str(sdk))
    from gen_esp32part import PartitionTable
    command=[sys.executable,'-m','esptool','--chip','esp32s31','--port',args.port,'--baud',str(args.baud)]
    with tempfile.TemporaryDirectory(prefix='mypaper-emulator-') as temporary:
        table_path=Path(temporary)/'device-partitions.bin'
        subprocess.run(command+['read-flash','0x8000','0x1000',str(table_path)],check=True)
        table=PartitionTable.from_binary(table_path.read_bytes())
        table.verify()
        check_table(table)
        print(f'Partition layout verified; writing ONLY emulator at {OFFSET:#x}.',flush=True)
        subprocess.run(command+['write-flash','--flash-size','keep',hex(OFFSET),str(args.image.resolve())],check=True)

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,FatalError,subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
