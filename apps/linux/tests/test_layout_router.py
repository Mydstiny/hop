import unittest
from hop_linux.layout import Device,Layout,Screen,sample_layout
from hop_linux.layout_router import route_edge

class LayoutRouterTests(unittest.TestCase):
    def test_multiple_peers_and_partial_edge_mapping(self):
        layout=sample_layout();mac,harmony,windows=layout.devices
        first=route_edge(layout,mac.id,'mac-main','right',65535,10000)
        self.assertEqual((first.device_id,first.screen_id,first.x),(harmony.id,'harmony-main',0))
        second=route_edge(layout,mac.id,'mac-main','right',65535,60000)
        self.assertEqual((second.device_id,second.screen_id,second.x),(windows.id,'windows-main',0))
        self.assertIsNone(route_edge(layout,mac.id,'mac-main','left',0,10000))
    def test_same_device_display_and_corner_only(self):
        layout=sample_layout();h=layout.devices[1]
        result=route_edge(layout,h.id,'harmony-main','right',65535,40000)
        self.assertEqual((result.device_id,result.screen_id,result.x),(h.id,'harmony-extra',0))
        a=Device('a'*64,'a','linux',0,0,(Screen('a',0,0,100,100),))
        b=Device('b'*64,'b','linux',100,100,(Screen('b',0,0,100,100),))
        self.assertIsNone(route_edge(Layout((a,b)),a.id,'a','right',65535,65535))
    def test_invalid_source_and_position(self):
        l=sample_layout();d=l.devices[0]
        with self.assertRaises(ValueError):route_edge(l,d.id,'missing','right',0,0)
        with self.assertRaises(ValueError):route_edge(l,d.id,'mac-main','right',-1,0)
