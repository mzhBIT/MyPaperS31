"""Validate built images, matching partition tables and installer write boundaries."""
from pathlib import Path
from types import SimpleNamespace,ModuleType
from unittest.mock import patch
import importlib.util
import json
import os
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('install_emulator',root/'tools/install_emulator.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)
sdk=Path(os.environ['IDF_PATH'])/'components/partition_table'
sys.path.insert(0,str(sdk))
from gen_esp32part import PartitionTable

class FakeTable(list):
    def verify(self):pass

def table():
    return FakeTable(SimpleNamespace(name=name,type=values[0],subtype=values[1],offset=values[2],size=values[3])
        for name,values in installer.CONTRACT.items())

installer.check_table(table())
bad=table();bad[2].offset=0x20000
try:installer.check_table(bad)
except ValueError:pass
else:raise AssertionError('Moved factory app accepted')
bad=FakeTable(table()[:3])
try:installer.check_table(bad)
except ValueError:pass
else:raise AssertionError('Old single-firmware table accepted')

for name in ('build-s31','build-s31-emulator'):
    data=(root/name/'partition_table/partition-table.bin').read_bytes()
    parsed=PartitionTable.from_binary(data);parsed.verify();installer.check_table(parsed)
    if name=='build-s31':system_table=data
    else:assert data==system_table,'System and emulator partition binaries differ'

image=root/'build-s31-emulator/MyPaperEmulatorS31.bin'
installer.check_image(image)
try:installer.check_image(root/'build-s31/MyPaperS31.bin')
except ValueError:pass
else:raise AssertionError('System image accepted as emulator')

manifest=json.loads((root/'build-s31-emulator/flasher_args.json').read_text(encoding='utf-8'))
assert {int(key,0):value for key,value in manifest['flash_files'].items()}=={0x810000:'MyPaperEmulatorS31.bin'}
for name in ('flash_args','app-flash_args'):
    lines=(root/'build-s31-emulator'/name).read_text(encoding='utf-8').splitlines()
    images=[line.split() for line in lines if line.strip().startswith('0x')]
    assert images==[['0x810000','MyPaperEmulatorS31.bin']]

fake_module=ModuleType('gen_esp32part')
commands=[]
def run(command,check):
    assert check;commands.append(command)
    if 'read-flash' in command:Path(command[-1]).write_bytes(b'mocked table')
fake_module.PartitionTable=SimpleNamespace(from_binary=lambda data:bad)
args=['install_emulator.py','--image',str(image),'--port','COM5']
with patch.dict(sys.modules,{'gen_esp32part':fake_module}),patch.object(sys,'argv',args),patch.object(installer.subprocess,'run',run):
    try:installer.main()
    except ValueError:pass
    else:raise AssertionError('Installer wrote to old device layout')
assert len(commands)==1 and 'read-flash' in commands[0]
fake_module.PartitionTable.from_binary=lambda data:table()
commands.clear()
with patch.dict(sys.modules,{'gen_esp32part':fake_module}),patch.object(sys,'argv',args),patch.object(installer.subprocess,'run',run):installer.main()
assert len(commands)==2 and commands[1][-3]=='keep' and commands[1][-2]=='0x810000'
with tempfile.TemporaryDirectory() as temporary:
    huge=Path(temporary)/'large.bin'
    with huge.open('wb') as output:output.truncate(installer.SIZE+1)
    try:installer.check_image(huge)
    except ValueError:pass
    else:raise AssertionError('Oversized image accepted')
print('Emulator install: real chip/image identity, identical layouts, app-only flash targets, old-device refusal and slot bounds PASS')
