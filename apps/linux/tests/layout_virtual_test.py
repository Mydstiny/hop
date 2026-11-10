"""Exercise the real GTK editor only on the fixture's isolated X server."""
import ctypes as C
import os
from pathlib import Path


def run():
    if os.environ.get("HOP_VIRTUAL_X_SERVER") != "1": raise RuntimeError("virtual X server required")
    from hop_linux.editor import LayoutEditor
    from hop_linux.layout import LayoutStore, sample_layout
    from hop_linux.layout_panel import GtkLayoutWindow, _ButtonEvent, _MotionEvent, STRINGS
    from hop_linux.profiles import PointerProfile, ProfileStore
    private = Path(os.environ["HOP_TEST_PRIVATE"])
    layout_file, profiles_file = private / "layout-test.json", private / "profiles-test.json"
    assert not layout_file.exists() and not profiles_file.exists()
    editor = LayoutEditor(sample_layout(), LayoutStore(layout_file), ProfileStore(profiles_file))
    panel = GtkLayoutWindow(editor, "zh")
    try:
        panel.show(); panel.iterate()
        assert panel.gtk.gtk_label_get_text(panel.status).decode() == STRINGS["zh"]["connected"]
        assert not layout_file.exists() and not profiles_file.exists()
        # Simulate callbacks directly on widgets in the isolated server, without OS input.
        scale, ox, oy = panel._transform()
        e = _ButtonEvent(); e.button=1; e.x=ox+200*scale; e.y=oy+200*scale
        assert panel._press(panel.canvas, C.cast(C.pointer(e), C.c_void_p)) == 1
        m = _MotionEvent(); m.x=e.x; m.y=e.y+100*scale
        assert panel._motion(panel.canvas, C.cast(C.pointer(m), C.c_void_p)) == 1
        assert panel._release(panel.canvas, C.cast(C.pointer(e), C.c_void_p)) == 1
        assert editor.layout.devices[0].y == 100
        panel.gtk.gtk_combo_box_set_active(panel.combo, 1); panel._toggle()
        assert editor.layout.devices[1].id in editor.expanded
        panel.save_virtual_screenshot(private / "layout-preview-zh.png")
        profile = PointerProfile(1.5, 1.25, .75, True, False)
        editor.set_profile(editor.selected, profile)
        assert ProfileStore(profiles_file).get(editor.selected) == profile
        panel._save()
        assert LayoutStore(layout_file).load().to_data() == editor.layout.to_data()
    finally:
        panel.close()
    print("PASS: isolated GTK layout rendering, drag, expansion, per-device settings persistence and explicit layout save")
