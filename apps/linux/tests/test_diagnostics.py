import unittest
from hop_linux.diagnostics import inventory


class DiagnosticsTests(unittest.TestCase):
    def test_x11_library_presence_does_not_claim_product_readiness(self):
        result = inventory({"DISPLAY": ":0"}, lambda n: "present" if n in ("X11", "Xtst") else None)
        self.assertEqual(result["desktop_session"], "x11")
        self.assertTrue(result["x11_runtime_libraries_present"])
        self.assertFalse(result["product_ready"])
        self.assertFalse(result["input_capture_tested"])
        self.assertFalse(result["network_protocol_integrated"])
    def test_wayland_takes_precedence_over_xwayland_display(self):
        result = inventory({"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0"}, lambda _: "present")
        self.assertEqual(result["desktop_session"], "wayland")
        self.assertIn("not implemented", result["note"])
    def test_headless_missing_libraries(self):
        result = inventory({}, lambda _: None)
        self.assertEqual(result["desktop_session"], "headless")
        self.assertFalse(result["x11_runtime_libraries_present"])
        self.assertFalse(result["display_opened"])
