import unittest
import select_offline_map_region as r


class IntersectionTests(unittest.TestCase):
    def test_concave_polygon_not_its_bbox(self):
        polygon=[[(0,0),(4,0),(4,1),(1,1),(1,4),(0,4),(0,0)]]
        self.assertFalse(r.intersects(polygon,(2,2,3,3)))
        self.assertTrue(r.intersects(polygon,(0.2,2,0.8,3)))
        self.assertTrue(r.intersects(polygon,(-1,-1,5,5)))
        self.assertTrue(r.intersects(polygon,(1,1,2,2)))

    def test_hole_and_island(self):
        polygon=[[(0,0),(8,0),(8,8),(0,8),(0,0)],
                 [(2,2),(6,2),(6,6),(2,6),(2,2)],
                 [(10,10),(11,10),(11,11),(10,11),(10,10)]]
        self.assertFalse(r.intersects(polygon,(3,3,5,5)))
        self.assertTrue(r.intersects(polygon,(10.2,10.2,10.8,10.8)))
        self.assertFalse(r.intersects(polygon,(8.1,8.1,9,9)))

    def test_edge_crossing_without_inside_corners(self):
        polygon=[[(0,1),(3,1),(3,2),(0,2),(0,1)]]
        self.assertTrue(r.intersects(polygon,(1,0,2,3)))
        self.assertFalse(r.intersects(polygon,(4,0,5,3)))


if __name__=='__main__':
    unittest.main()
