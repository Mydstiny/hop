"""Native GTK3 configuration preview, with no network or input side effects.

The interface intentionally exposes unavailable operations as disabled until a
real authenticated session/controller is integrated. No GTK call is made at
import time. GTK3 is loaded dynamically and no Python GUI package is required.
"""
from __future__ import annotations
import ctypes as C
import ctypes.util
import os
import time


class _Allocation(C.Structure):
    _fields_ = [("x", C.c_int), ("y", C.c_int), ("width", C.c_int), ("height", C.c_int)]

TEXTS = {
    "zh": {
        "title": "Hop · 设备与设置", "heading": "让键鼠跨越设备", "subtitle": "在局域网中连接你的电脑",
        "status": "开发预览 · 暂不可连接", "devices": "设备布局", "empty": "还没有已连接的设备",
        "empty_hint": "连接功能完成后，可以在这里排列设备和显示器。",
        "settings": "键鼠与剪贴板", "settings_hint": "选择设备后，可分别调整鼠标灵敏度、滚轮方向和键盘映射。",
        "refresh": "查找设备", "configure": "设备设置", "pause": "暂停共享", "quit": "关闭",
        "language": "English", "note": "灵敏度为软件倍率，不会更改鼠标硬件 DPI。当前预览尚未接入连接。",
    },
    "en": {
        "title": "Hop · Devices & Settings", "heading": "One keyboard, more computers", "subtitle": "Connect your computers on the local network",
        "status": "Development preview · connections unavailable", "devices": "Device layout", "empty": "No connected devices yet",
        "empty_hint": "Arrange computers and displays here when connections are available.",
        "settings": "Input & clipboard", "settings_hint": "Select a device to adjust pointer speed, scrolling, and keyboard mapping.",
        "refresh": "Find devices", "configure": "Device settings", "pause": "Pause sharing", "quit": "Close",
        "language": "中文", "note": "Sensitivity is a software multiplier; it does not change hardware DPI. Connections are not yet integrated.",
    },
}


def default_language(env=None):
    env = os.environ if env is None else env
    value = env.get("LC_ALL") or env.get("LC_MESSAGES") or env.get("LANG") or "en"
    return "zh" if value.lower().startswith("zh") else "en"


def labels(language):
    if language not in TEXTS: raise ValueError("unsupported interface language")
    return dict(TEXTS[language])


def _bind(lib, name, restype, *args):
    fn = getattr(lib, name); fn.restype = restype; fn.argtypes = list(args); return fn


class GtkPanel:
    """Small native preview window; call run() only after explicit user action."""
    def __init__(self, language=None, *, quit_on_destroy=True):
        gtk, gdk, gobject = (ctypes.util.find_library(name) for name in ("gtk-3", "gdk-3", "gobject-2.0"))
        if not all((gtk, gdk, gobject)):
            raise RuntimeError("GTK3 runtime libraries are required")
        self.gtk, self.gdk, self.gobject = C.CDLL(gtk), C.CDLL(gdk), C.CDLL(gobject)
        self._bind()
        if not self.gtk.gtk_init_check(None, None):
            raise RuntimeError("GTK could not connect to a desktop display")
        self.language = language or default_language()
        labels(self.language)  # validate
        self.window = self.gtk.gtk_window_new(0)
        if not self.window: raise RuntimeError("GTK could not create a window")
        self.gtk.gtk_window_set_default_size(self.window, 560, 440)
        self.gtk.gtk_container_set_border_width(self.window, 24)
        self._callbacks = []
        if quit_on_destroy:self._connect(self.window, "destroy", self._quit_if_running)
        self.root = self.gtk.gtk_box_new(1, 14)
        self.gtk.gtk_container_add(self.window, self.root)
        header = self.gtk.gtk_box_new(0, 12)
        brand = self.gtk.gtk_label_new(b"Hop")
        self.gtk.gtk_widget_set_name(brand, b"hop-brand")
        self.gtk.gtk_label_set_xalign(brand, 0)
        self.gtk.gtk_box_pack_start(header, brand, True, True, 0)
        self.language_button = self.gtk.gtk_button_new_with_label(b"")
        self.gtk.gtk_box_pack_end(header, self.language_button, False, False, 0)
        self._connect(self.language_button, "clicked", self.toggle_language)
        self.gtk.gtk_box_pack_start(self.root, header, False, False, 0)
        self.widgets = {}
        for key in ("heading", "subtitle", "status", "devices", "empty", "empty_hint", "settings", "settings_hint", "note"):
            widget = self.gtk.gtk_label_new(b"")
            self.widgets[key] = widget
            self.gtk.gtk_label_set_xalign(widget, 0)
            self.gtk.gtk_label_set_line_wrap(widget, True)
            self.gtk.gtk_widget_set_name(widget, ("hop-" + key.replace("_", "-")).encode())
            self.gtk.gtk_box_pack_start(self.root, widget, False, False, 0)
        actions = self.gtk.gtk_box_new(0, 8)
        for key in ("refresh", "configure", "pause"):
            button = self.gtk.gtk_button_new_with_label(b"")
            self.widgets[key] = button
            self.gtk.gtk_widget_set_sensitive(button, False)
            self.gtk.gtk_box_pack_start(actions, button, False, False, 0)
        self.widgets["quit"] = self.gtk.gtk_button_new_with_label(b"")
        self._connect(self.widgets["quit"], "clicked", lambda: self.gtk.gtk_widget_destroy(self.window))
        self.gtk.gtk_box_pack_end(actions, self.widgets["quit"], False, False, 0)
        self.gtk.gtk_box_pack_end(self.root, actions, False, False, 0)
        self._style()
        self.set_language(self.language)

    def _bind(self):
        g, d, o = self.gtk, self.gdk, self.gobject
        P, I, U, B, S = C.c_void_p, C.c_int, C.c_uint, C.c_char_p, C.c_ssize_t
        _bind(g, "gtk_init_check", I, P, P)
        _bind(g, "gtk_window_new", P, I)
        _bind(g, "gtk_window_set_default_size", None, P, I, I)
        _bind(g, "gtk_window_set_title", None, P, B)
        _bind(g, "gtk_container_set_border_width", None, P, U)
        _bind(g, "gtk_container_add", None, P, P)
        _bind(g, "gtk_box_new", P, I, I)
        _bind(g, "gtk_box_pack_start", None, P, P, I, I, U)
        _bind(g, "gtk_box_pack_end", None, P, P, I, I, U)
        _bind(g, "gtk_label_new", P, B)
        _bind(g, "gtk_label_set_text", None, P, B)
        _bind(g, "gtk_label_get_text", B, P)
        _bind(g, "gtk_label_set_xalign", None, P, C.c_float)
        _bind(g, "gtk_label_set_line_wrap", None, P, I)
        _bind(g, "gtk_button_new_with_label", P, B)
        _bind(g, "gtk_button_set_label", None, P, B)
        _bind(g, "gtk_button_clicked", None, P)
        _bind(g, "gtk_widget_set_name", None, P, B)
        _bind(g, "gtk_widget_set_sensitive", None, P, I)
        _bind(g, "gtk_widget_get_sensitive", I, P)
        _bind(g, "gtk_widget_show_all", None, P)
        _bind(g, "gtk_widget_destroy", None, P)
        _bind(g, "gtk_widget_get_window", P, P)
        _bind(g, "gtk_widget_get_allocated_width", I, P)
        _bind(g, "gtk_widget_get_allocated_height", I, P)
        _bind(g, "gtk_widget_get_allocation", None, P, C.POINTER(_Allocation))
        _bind(g, "gtk_widget_size_allocate", None, P, C.POINTER(_Allocation))
        _bind(g, "gtk_widget_draw", None, P, P)
        _bind(g, "gtk_events_pending", I)
        _bind(g, "gtk_main_iteration_do", I, I)
        _bind(g, "gtk_main", None)
        _bind(g, "gtk_main_level", U)
        _bind(g, "gtk_main_quit", None)
        _bind(g, "gtk_css_provider_new", P)
        _bind(g, "gtk_css_provider_load_from_data", I, P, B, S, P)
        _bind(g, "gtk_style_context_add_provider_for_screen", None, P, P, U)
        _bind(d, "gdk_screen_get_default", P)
        _bind(o, "g_signal_connect_data", C.c_ulong, P, B, P, P, P, I)
        _bind(o, "g_object_unref", None, P)
        cairo = ctypes.util.find_library("cairo")
        if not cairo: raise RuntimeError("Cairo runtime library required")
        self.cairo = C.CDLL(cairo)
        _bind(self.cairo, "cairo_image_surface_create", P, I, I, I)
        _bind(self.cairo, "cairo_create", P, P)
        _bind(self.cairo, "cairo_destroy", None, P)
        _bind(self.cairo, "cairo_surface_destroy", None, P)
        _bind(self.cairo, "cairo_surface_flush", None, P)
        _bind(self.cairo, "cairo_image_surface_get_data", C.POINTER(C.c_ubyte), P)
        _bind(self.cairo, "cairo_image_surface_get_stride", I, P)
        _bind(self.cairo, "cairo_surface_write_to_png", I, P, B)

    def _connect(self, widget, signal, callback):
        cb_type = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p)
        cb = cb_type(lambda _widget, _data: callback())
        self._callbacks.append(cb)
        self.gobject.g_signal_connect_data(widget, signal.encode(), C.cast(cb, C.c_void_p), None, None, 0)

    def _style(self):
        css = b'''#hop-brand { font-size: 25px; font-weight: 800; color: #345bde; }
#hop-heading { font-size: 23px; font-weight: 700; }
#hop-subtitle, #hop-empty-hint, #hop-settings-hint, #hop-note { color: #687385; font-size: 12px; }
#hop-status { color: #9a6613; font-weight: 600; padding: 8px; background: #fff4d5; border-radius: 7px; }
#hop-devices, #hop-settings { font-size: 15px; font-weight: 700; }
#hop-empty { font-weight: 600; }
button { border-radius: 7px; padding: 5px 10px; }'''
        provider = self.gtk.gtk_css_provider_new()
        if self.gtk.gtk_css_provider_load_from_data(provider, css, len(css), None):
            self.gtk.gtk_style_context_add_provider_for_screen(self.gdk.gdk_screen_get_default(), provider, 600)
        self.gobject.g_object_unref(provider)

    def set_language(self, language):
        data = labels(language); self.language = language
        self.gtk.gtk_window_set_title(self.window, data["title"].encode())
        self.gtk.gtk_button_set_label(self.language_button, data["language"].encode())
        for key, widget in self.widgets.items():
            setter = self.gtk.gtk_button_set_label if key in ("refresh", "configure", "pause", "quit") else self.gtk.gtk_label_set_text
            setter(widget, data[key].encode())

    def toggle_language(self): self.set_language("en" if self.language == "zh" else "zh")
    def _quit_if_running(self):
        if self.gtk.gtk_main_level(): self.gtk.gtk_main_quit()
    def show(self): self.gtk.gtk_widget_show_all(self.window)
    def iterate(self):
        while self.gtk.gtk_events_pending(): self.gtk.gtk_main_iteration_do(False)
    def run(self): self.show(); self.gtk.gtk_main()
    def close(self):
        if self.window:
            self.gtk.gtk_widget_destroy(self.window)
            self.window = None

    def save_virtual_screenshot(self, path):
        """Test-only: only available inside the explicitly created Xvfb fixture."""
        if os.environ.get("HOP_VIRTUAL_X_SERVER") != "1": raise RuntimeError("virtual X server required")
        # GTK frame-clock drawing is asynchronous; a first nonblocking event
        # iteration alone can capture an entirely black, unpainted surface.
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            self.iterate()
            time.sleep(0.01)
        allocation = _Allocation()
        self.gtk.gtk_widget_get_allocation(self.window, C.byref(allocation))
        self.gtk.gtk_widget_size_allocate(self.window, C.byref(allocation))
        win = self.gtk.gtk_widget_get_window(self.window)
        width = self.gtk.gtk_widget_get_allocated_width(self.window)
        height = self.gtk.gtk_widget_get_allocated_height(self.window)
        if not win or width <= 0 or height <= 0: raise RuntimeError("GTK preview has no realized window")
        # Draw the real allocated GTK widget to an image surface. This is an
        # offscreen content render, not a screenshot of a desktop interaction.
        image = self.cairo.cairo_image_surface_create(0, width, height)  # ARGB32
        context = self.cairo.cairo_create(image)
        if not image or not context: raise RuntimeError("failed to create offscreen GTK surface")
        try:
            self.gtk.gtk_widget_draw(self.window, context)
            self.cairo.cairo_surface_flush(image)
            pixels = self.cairo.cairo_image_surface_get_data(image)
            stride = self.cairo.cairo_image_surface_get_stride(image)
            if not pixels: raise RuntimeError("invalid virtual GTK image")
            colors = set()
            for y in range(0, height, max(1, height // 40)):
                for x in range(0, width, max(1, width // 40)):
                    i = y * stride + x * 4
                    colors.add((pixels[i], pixels[i + 1], pixels[i + 2]))
            if len(colors) < 4: raise RuntimeError("GTK screenshot was not painted")
            if self.cairo.cairo_surface_write_to_png(image, os.fsencode(path)) != 0:
                raise RuntimeError("failed to save virtual GTK screenshot")
        finally:
            self.cairo.cairo_destroy(context)
            self.cairo.cairo_surface_destroy(image)
