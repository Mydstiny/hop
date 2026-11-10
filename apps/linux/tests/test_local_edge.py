import unittest
from hop_linux.local_edge import EdgeSampler


class EdgeSamplerTests(unittest.TestCase):
    def test_only_movement_from_inside_to_edge_triggers(self):
        s=EdgeSampler(((7,0,0,100,80),))
        self.assertIsNone(s.sample(99,20))
        self.assertIsNone(s.sample(99,20))
        self.assertIsNone(s.sample(90,20))
        e=s.sample(99,20)
        self.assertEqual((e.screen_id,e.direction,e.x),(7,'right',65535))
        self.assertIsNone(s.sample(99,20))
        self.assertIsNone(s.sample(90,20))
    def test_multi_display_and_ambiguous_corners(self):
        s=EdgeSampler(((7,0,0,100,80),(8,100,0,100,80)))
        s.sample(120,20);e=s.sample(100,20)
        self.assertEqual((e.screen_id,e.direction,e.x),(8,'left',0))
        s.sample(120,20)
        self.assertIsNone(s.sample(100,0))
        self.assertIsNone(s.sample(99,0)) # crossing between local monitors isn't an edge approach
    def test_bad_geometry_and_position_rejected(self):
        with self.assertRaises(ValueError):EdgeSampler(((1,0,0,1,3),))
        s=EdgeSampler(((1,0,0,2,2),))
        with self.assertRaises(ValueError):s.sample(True,0)
