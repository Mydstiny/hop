"""GTK window smoke test only inside the fixture's isolated Xvfb server."""
import os
from pathlib import Path


def run():
    if os.environ.get("HOP_VIRTUAL_X_SERVER") != "1": raise RuntimeError("virtual X server required")
    from hop_linux.panel import GtkPanel, labels
    panel = GtkPanel("zh")
    try:
        panel.show(); panel.iterate()
        assert panel.gtk.gtk_label_get_text(panel.widgets["status"]).decode() == labels("zh")["status"]
        assert all(not panel.gtk.gtk_widget_get_sensitive(panel.widgets[k]) for k in ("refresh", "configure", "pause"))
        screenshot = Path(os.environ["HOP_TEST_PRIVATE"]) / "gtk-preview-zh.png"
        panel.save_virtual_screenshot(screenshot)
        panel.toggle_language(); panel.iterate()
        assert panel.gtk.gtk_label_get_text(panel.widgets["heading"]).decode() == labels("en")["heading"]
        screenshot_en = Path(os.environ["HOP_TEST_PRIVATE"]) / "gtk-preview-en.png"
        panel.save_virtual_screenshot(screenshot_en)
        assert screenshot.stat().st_size > 1000 and screenshot_en.stat().st_size > 1000
    finally:
        panel.close()
    print("PASS: isolated GTK3 configuration preview, bilingual labels, disabled unavailable actions and screenshots")
