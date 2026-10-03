import argparse
import json
from pathlib import Path
import tempfile
import unittest
import struct
import zlib
from PIL import Image
import split_offline_map as s
import build_offline_map as m


class SplitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.tiles = self.root/'tiles'
        self.out = self.root/'packs'
        self.tiles.mkdir()
        self.out.mkdir()
        for z in (1,2,3):
            for x in range(1 << z):
                for y in range(1 << z):
                    p = self.tiles/str(z)/str(x)/f'{y}.png'
                    p.parent.mkdir(parents=True,exist_ok=True)
                    Image.new('L',(256,256),x*10+y).save(p)

    def tearDown(self):
        self.temp.cleanup()

    def test_grid_overlap_limit_and_resume(self):
        db = s.catalog(self.tiles,self.out,[1,2,3])
        try:
            cap = int(1.6*2**20)
            packs = s.plan(db,[1,2,3],2,4,1,cap)
            details = [p for p in packs if p['file'] != 'overview.mpm']
            self.assertTrue(details)
            self.assertTrue(all(p['bytes'] <= cap for p in details))
            keys = set()
            total = 0
            for p in details:
                rows = s.pack_rows(db,p,2,1)
                keys.update(tuple(row[:3]) for row in rows)
                total += len(rows)
            self.assertEqual(len(keys),80)
            self.assertGreater(total,len(keys))
            info = details[0]
            s.build_one(db,self.out,info,2,1)
            target = self.out/info['file']
            stamp = target.stat().st_mtime_ns
            s.build_one(db,self.out,info,2,1)
            self.assertEqual(stamp,target.stat().st_mtime_ns)
            with target.open('r+b') as f:
                f.seek(-1,2); f.write(b'\xff')
            with self.assertRaisesRegex(ValueError,'CRC'):
                s.build_one(db,self.out,info,2,1)
        finally:
            db.close()

    def test_incomplete_export_does_not_publish(self):
        (self.tiles/'3'/'7'/'7.png').unlink()
        args = argparse.Namespace(tiles=self.tiles,output=self.out,min_zoom=1,max_zoom=3,
            split_zoom=2,block=2,halo=0,max_pack_mib=1,plan_only=False,
            extent_3857=[-20037508.342789244,20037508.342789244,
                         -20037508.342789244,20037508.342789244])
        with self.assertRaisesRegex(ValueError,'Missing zooms'):
            s.run(args)
        self.assertFalse((self.out/'manifest.json').exists())
        self.assertFalse(list(self.out.glob('*.mpm')))

    def test_expected_rectangle(self):
        half = 20037508.342789244
        self.assertEqual(s.expected_rectangle([-half,half,-half,half],3),(0,0,7,7))

    def test_complete_v2_catalog_and_resume(self):
        half = 20037508.342789244
        args = argparse.Namespace(tiles=self.tiles,output=self.out,min_zoom=1,max_zoom=3,
            split_zoom=2,block=2,halo=0,max_pack_mib=2,plan_only=False,
            extent_3857=[-half,half,-half,half],version=2,workers=4)
        s.run(args)
        info=json.loads((self.out/'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(info['format'],'MPM-v2-packset')
        self.assertFalse(info['snapshot_only'])
        self.assertTrue(all(p['actual_bytes'] < p['bytes'] for p in info['packs']))
        data=(self.out/'catalog.mpc').read_bytes()
        magic,version,header,record,count,crc,flags,reserved=struct.unpack_from('<8sHHIIIII',data)
        self.assertEqual((magic,version,header,record,count,flags,reserved),
                         (b'MPCAT001',1,32,136,len(info['packs']),1,0))
        self.assertEqual(zlib.crc32(data[32:]),crc)
        stamps={p.name:p.stat().st_mtime_ns for p in self.out.glob('*.mpm')}
        s.run(args)
        self.assertEqual(stamps,{p.name:p.stat().st_mtime_ns for p in self.out.glob('*.mpm')})


if __name__ == '__main__':
    unittest.main()
