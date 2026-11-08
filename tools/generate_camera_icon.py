"""Render the supplied Feather camera SVG into the home icon table."""
from pathlib import Path
import math
import re
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw
from generate_home_status_icons import arc_points
from add_home_icon import pack, format_array

ROOT = Path(__file__).resolve().parents[1]
SCALE = 20

def main():
    svg = ET.parse(ROOT/'Assets/feather/camera.svg').getroot()
    image = Image.new('L', (24*SCALE, 24*SCALE), 255)
    draw = ImageDraw.Draw(image)
    stroke = float(svg.get('stroke-width', '2'))*SCALE
    for node in svg:
        kind = node.tag.split('}')[-1]
        points = []
        if kind == 'circle':
            cx,cy,r = (float(node.get(k)) for k in ('cx','cy','r'))
            points = [(cx+r*math.cos(i*2*math.pi/160),cy+r*math.sin(i*2*math.pi/160)) for i in range(161)]
        elif kind == 'path':
            tokens = re.findall(r'[A-Za-z]|[-+]?(?:\d*\.\d+|\d+)',node.get('d'))
            at=0; x=y=0; start=(0,0); command=''
            while at<len(tokens):
                if tokens[at].isalpha(): command=tokens[at]; at+=1
                upper=command.upper(); relative=command.islower()
                if upper=='Z': points.append(start); x,y=start; continue
                count={'M':2,'L':2,'H':1,'V':1,'A':7}[upper]
                values=[float(v) for v in tokens[at:at+count]]; at+=count
                if upper=='A':
                    rx,ry,rot,large,sweep,nx,ny=values
                    if relative: nx+=x; ny+=y
                    points.extend(arc_points(x,y,rx,ry,rot,large,sweep,nx,ny)); x,y=nx,ny
                else:
                    if upper in ('M','L'):
                        nx,ny=values
                        if relative: nx+=x; ny+=y
                    elif upper=='H': nx,ny=(x+values[0] if relative else values[0]),y
                    else: nx,ny=x,(y+values[0] if relative else values[0])
                    x,y=nx,ny; points.append((x,y))
                    if upper=='M': start=(x,y); command='l' if relative else 'L'
        else: raise ValueError(kind)
        scaled=[(round(x*SCALE),round(y*SCALE)) for x,y in points]
        draw.line(scaled,fill=0,width=round(stroke),joint='curve')
        for x,y in scaled:
            r=stroke/2; draw.ellipse((x-r,y-r,x+r,y+r),fill=0)
    # Match the existing home icon footprint, leaving a small white margin.
    tile=Image.new('L',(150,150),255)
    tile.paste(image.resize((130,130),Image.Resampling.LANCZOS),(10,10))
    path=ROOT/'Assets/camera-home.png'; tile.save(path)
    array=format_array('icon_camera_data',pack(path))
    output=ROOT/'main/ui/home_icons_1bpp.cpp'
    text=output.read_text(encoding='utf-8')
    if 'const uint8_t icon_camera_data[' not in text:
        text=text.replace('\n} // namespace\n','\n'+array+'\n\n} // namespace\n',1)
    case='    case HomeIconId::Camera: return icon_camera_data;\n'
    if case not in text:text=text.replace('    default: return nullptr;\n',case+'    default: return nullptr;\n',1)
    output.write_text(text,encoding='utf-8')

if __name__=='__main__': main()
