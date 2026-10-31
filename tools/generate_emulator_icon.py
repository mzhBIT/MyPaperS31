"""Generate the small home controller icon without external raster dependencies."""
from pathlib import Path
import math

root=Path(__file__).resolve().parents[1]
data=bytearray(19*150)
def ink(x,y):
    # Rounded controller outline, D-pad and two face buttons.
    qx=abs(x-75)-36; qy=abs(y-75)-16
    rounded=math.hypot(max(qx,0),max(qy,0))+min(max(qx,qy),0)-14
    return abs(rounded)<=2 or (42<=x<=68 and abs(y-75)<=2) or \
        (abs(x-55)<=2 and 62<=y<=88) or \
        abs(math.hypot(x-99,y-66)-4)<=2 or abs(math.hypot(x-111,y-82)-4)<=2
for y in range(150):
    for x in range(150):
        if ink(x+.5,y+.5):data[y*19+x//8]|=0x80>>(x&7)
lines=['#pragma once','#include <cstdint>','static const uint8_t icon_emulator_data[HOME_ICON_1BPP_DATA_SIZE] = {']
for i in range(0,len(data),16):lines.append('    '+', '.join(f'0x{v:02X}' for v in data[i:i+16])+',')
lines.append('};')
(root/'main/ui/emulator_icon_1bpp.h').write_text('\n'.join(lines)+'\n',encoding='utf-8')
(root/'Assets/emulator.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="150" height="150" viewBox="0 0 150 150">
<g fill="none" stroke="black" stroke-width="4" stroke-linecap="round">
<rect x="25" y="45" width="100" height="60" rx="14"/>
<path d="M42 75h26M55 62v26"/><circle cx="99" cy="66" r="4"/><circle cx="111" cy="82" r="4"/>
</g></svg>''',encoding='utf-8')
