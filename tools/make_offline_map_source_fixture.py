"""Create a small, deterministic native-reader fixture in a NEW directory."""
import sys
from pathlib import Path
from PIL import Image
import build_offline_map as m
import split_offline_map as s

root=Path(sys.argv[1])
root.mkdir(parents=True,exist_ok=False)
all_tiles=[]
for x in range(8):
    for y in range(8):
        path=root/f'{x}-{y}.png'
        Image.new('L',(256,256),(x+y)*17).save(path)
        all_tiles.append(((3,x,y),path))
packs=[]
for name,start,stop,core in [('left',0,7,(3,0,0,4,8)),('right',1,8,(3,4,0,8,8))]:
    tiles=[t for t in all_tiles if start<=t[0][1]<stop]
    m.build_pack(tiles,root/f'{name}.mpm',version=2,workers=4)
    packs.append(dict(file=f'{name}.mpm',zooms=[3],core_bounds=s.region_bounds(core),
                      bounds=s.region_bounds((3,start,0,stop,8))))
s.write_catalog(root,packs)
