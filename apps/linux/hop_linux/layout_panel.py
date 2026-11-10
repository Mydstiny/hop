"""GTK3 multi-device layout editor for simulated data; no networking."""
from __future__ import annotations

import ctypes as C
import ctypes.util
import os
import time

from .editor import LayoutEditor
from .profiles import PointerProfile


class _Allocation(C.Structure):
    _fields_ = [("x", C.c_int), ("y", C.c_int), ("width", C.c_int), ("height", C.c_int)]


class _ButtonEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("window", C.c_void_p), ("send_event", C.c_byte),
                ("time", C.c_uint), ("x", C.c_double), ("y", C.c_double), ("axes", C.c_void_p),
                ("state", C.c_uint), ("button", C.c_uint), ("device", C.c_void_p),
                ("x_root", C.c_double), ("y_root", C.c_double)]


class _MotionEvent(C.Structure):
    _fields_ = [("type", C.c_int), ("window", C.c_void_p), ("send_event", C.c_byte),
                ("time", C.c_uint), ("x", C.c_double), ("y", C.c_double), ("axes", C.c_void_p),
                ("state", C.c_uint), ("is_hint", C.c_short), ("device", C.c_void_p),
                ("x_root", C.c_double), ("y_root", C.c_double)]


STRINGS = {
    "zh": {"title": "Hop · 布局示例", "heading": "设备布局", "subtitle": "模拟设备 · 拖动设备或展开后拖动显示器",
           "expand": "展开 / 收起显示器", "settings": "设备设置", "save": "保存布局", "close": "关闭",
           "connected": "边缘相邻关系有效", "disconnected": "部分设备没有相邻边，无法穿越", "overlap": "屏幕重叠，松开后将恢复原位置",
           "saved": "布局已保存到指定文件", "unsaved": "仅本地预览；没有指定保存文件", "bad_save": "保存失败：请检查文件和权限",
           "prefs": "设备的软件灵敏度", "movement": "鼠标移动倍率", "hscroll": "水平滚轮倍率", "vscroll": "垂直滚轮倍率", "frequency": "鼠标移动目标发送频率（Hz）",
           "reverse_x": "反转水平滚轮", "reverse_y": "反转垂直滚轮", "dpi": "软件倍率不会改变鼠标硬件 DPI。", "cancel": "取消", "apply": "应用"},
    "en": {"title": "Hop · Layout preview", "heading": "Device layout", "subtitle": "Simulated devices · drag computers or expand to move displays",
           "expand": "Expand / collapse displays", "settings": "Device settings", "save": "Save layout", "close": "Close",
           "connected": "Devices have valid adjacent edges", "disconnected": "Some devices have no reachable adjacent edge", "overlap": "Displays overlap; the move will be reverted",
           "saved": "Layout saved to the selected file", "unsaved": "Local preview only; no save file selected", "bad_save": "Could not save; check the file and permissions",
           "prefs": "Device sensitivity", "movement": "Pointer movement", "hscroll": "Horizontal scroll", "vscroll": "Vertical scroll", "frequency": "Target pointer update rate (Hz)",
           "reverse_x": "Reverse horizontal scroll", "reverse_y": "Reverse vertical scroll", "dpi": "Software multipliers do not change hardware DPI.", "cancel": "Cancel", "apply": "Apply"},
}


def _bind(lib, name, result, *args):
    f = getattr(lib, name); f.restype = result; f.argtypes = list(args); return f


class GtkLayoutWindow:
    def __init__(self, editor, language="zh", *, simulated=True):
        if not isinstance(editor, LayoutEditor) or language not in STRINGS: raise ValueError("invalid editor or language")
        names = [ctypes.util.find_library(x) for x in ("gtk-3", "gobject-2.0", "cairo", "pangocairo-1.0", "pango-1.0")]
        if not all(names): raise RuntimeError("GTK3, Cairo and Pango runtime libraries are required")
        self.gtk, self.gobject, self.cairo, self.pangocairo, self.pango = [C.CDLL(x) for x in names]
        self._bind()
        if not self.gtk.gtk_init_check(None, None): raise RuntimeError("GTK could not connect to a display")
        self.editor, self.language, self.text = editor, language, STRINGS[language]
        if not simulated:
            self.text=dict(self.text)
            self.text['title']='Hop · 设备布局' if language=='zh' else 'Hop · Device layout'
            self.text['subtitle']='拖动设备，或展开后拖动显示器' if language=='zh' else 'Drag computers, or expand to move displays'
        self._callbacks = []
        self._drag = None
        self.window = self.gtk.gtk_window_new(0)
        self.gtk.gtk_window_set_title(self.window, self.text["title"].encode())
        self.gtk.gtk_window_set_default_size(self.window, 900, 620)
        self.gtk.gtk_container_set_border_width(self.window, 20)
        self._signal(self.window, "destroy", self._quit)
        root = self.gtk.gtk_box_new(1, 12); self.gtk.gtk_container_add(self.window, root)
        title = self.gtk.gtk_label_new(self.text["heading"].encode()); self.gtk.gtk_label_set_xalign(title, 0)
        self.gtk.gtk_widget_set_name(title, b"hop-layout-title"); self.gtk.gtk_box_pack_start(root, title, False, False, 0)
        subtitle = self.gtk.gtk_label_new(self.text["subtitle"].encode()); self.gtk.gtk_label_set_xalign(subtitle, 0)
        self.gtk.gtk_box_pack_start(root, subtitle, False, False, 0)
        bar = self.gtk.gtk_box_new(0, 8); self.gtk.gtk_box_pack_start(root, bar, False, False, 0)
        self.combo = self.gtk.gtk_combo_box_text_new()
        for d in self.editor.layout.devices: self.gtk.gtk_combo_box_text_append_text(self.combo, d.name.encode())
        self.gtk.gtk_combo_box_set_active(self.combo, 0)
        self._signal(self.combo, "changed", self._changed)
        self.gtk.gtk_box_pack_start(bar, self.combo, True, True, 0)
        self.expand_button = self._button(bar, "expand", self._toggle)
        self.settings_button = self._button(bar, "settings", self._settings)
        self.canvas = self.gtk.gtk_drawing_area_new()
        self.gtk.gtk_widget_set_name(self.canvas, b"hop-layout-canvas")
        self.gtk.gtk_widget_set_size_request(self.canvas, 600, 360)
        self.gtk.gtk_widget_set_hexpand(self.canvas, True); self.gtk.gtk_widget_set_vexpand(self.canvas, True)
        self.gtk.gtk_widget_add_events(self.canvas, (1 << 2) | (1 << 8) | (1 << 9))
        self._event_signal(self.canvas, "draw", self._draw)
        self._event_signal(self.canvas, "button-press-event", self._press)
        self._event_signal(self.canvas, "motion-notify-event", self._motion)
        self._event_signal(self.canvas, "button-release-event", self._release)
        self.gtk.gtk_box_pack_start(root, self.canvas, True, True, 0)
        self.status = self.gtk.gtk_label_new(b""); self.gtk.gtk_label_set_xalign(self.status, 0)
        self.gtk.gtk_box_pack_start(root, self.status, False, False, 0)
        self.note = self.gtk.gtk_label_new(b""); self.gtk.gtk_label_set_xalign(self.note, 0)
        self.gtk.gtk_box_pack_start(root, self.note, False, False, 0)
        bottom = self.gtk.gtk_box_new(0, 8); self.gtk.gtk_box_pack_start(root, bottom, False, False, 0)
        self.save_button = self._button(bottom, "save", self._save)
        self.close_button = self._button(bottom, "close", self.close)
        self._style(); self._refresh()

    def _bind(self):
        g, o, c, pc, p = self.gtk, self.gobject, self.cairo, self.pangocairo, self.pango
        P, I, U, B, D, S = C.c_void_p, C.c_int, C.c_uint, C.c_char_p, C.c_double, C.c_ssize_t
        for name, result, args in (
            ("gtk_init_check",I,(P,P)),("gtk_window_new",P,(I,)),("gtk_window_set_title",None,(P,B)),
            ("gtk_window_set_default_size",None,(P,I,I)),("gtk_window_set_transient_for",None,(P,P)),
            ("gtk_container_set_border_width",None,(P,U)),("gtk_container_add",None,(P,P)),
            ("gtk_box_new",P,(I,I)),("gtk_box_pack_start",None,(P,P,I,I,U)),
            ("gtk_label_new",P,(B,)),("gtk_label_set_text",None,(P,B)),("gtk_label_get_text",B,(P,)),("gtk_label_set_xalign",None,(P,C.c_float)),
            ("gtk_button_new_with_label",P,(B,)),("gtk_button_set_label",None,(P,B)),
            ("gtk_combo_box_text_new",P,()),("gtk_combo_box_text_append_text",None,(P,B)),("gtk_combo_box_set_active",None,(P,I)),("gtk_combo_box_get_active",I,(P,)),
            ("gtk_drawing_area_new",P,()),("gtk_widget_set_name",None,(P,B)),("gtk_widget_set_size_request",None,(P,I,I)),
            ("gtk_widget_set_hexpand",None,(P,I)),("gtk_widget_set_vexpand",None,(P,I)),("gtk_widget_add_events",None,(P,I)),
            ("gtk_widget_queue_draw",None,(P,)),("gtk_widget_set_sensitive",None,(P,I)),("gtk_widget_get_sensitive",I,(P,)),
            ("gtk_widget_show_all",None,(P,)),("gtk_widget_destroy",None,(P,)),("gtk_widget_draw",None,(P,P)),
            ("gtk_widget_get_allocated_width",I,(P,)),("gtk_widget_get_allocated_height",I,(P,)),
            ("gtk_widget_get_allocation",None,(P,C.POINTER(_Allocation))),("gtk_widget_size_allocate",None,(P,C.POINTER(_Allocation))),
            ("gtk_events_pending",I,()),("gtk_main_iteration_do",I,(I,)),("gtk_main",None,()),("gtk_main_level",U,()),("gtk_main_quit",None,()),
            ("gtk_dialog_new",P,()),("gtk_dialog_add_button",P,(P,B,I)),("gtk_dialog_get_content_area",P,(P,)),("gtk_dialog_run",I,(P,)),
            ("gtk_spin_button_new_with_range",P,(D,D,D)),("gtk_spin_button_set_value",None,(P,D)),("gtk_spin_button_get_value",D,(P,)),
            ("gtk_check_button_new_with_label",P,(B,)),("gtk_toggle_button_set_active",None,(P,I)),("gtk_toggle_button_get_active",I,(P,)),
            ("gtk_css_provider_new",P,()),("gtk_css_provider_load_from_data",I,(P,B,S,P)),("gtk_style_context_add_provider_for_screen",None,(P,P,U))):
            _bind(g,name,result,*args)
        self.gdk = C.CDLL(ctypes.util.find_library("gdk-3")); _bind(self.gdk,"gdk_screen_get_default",P)
        _bind(o,"g_signal_connect_data",C.c_ulong,P,B,P,P,P,I); _bind(o,"g_object_unref",None,P)
        for name,result,args in (("cairo_set_source_rgb",None,(P,D,D,D)),("cairo_rectangle",None,(P,D,D,D,D)),
            ("cairo_fill",None,(P,)),("cairo_stroke",None,(P,)),("cairo_set_line_width",None,(P,D)),
            ("cairo_move_to",None,(P,D,D)),("cairo_paint",None,(P,)),("cairo_image_surface_create",P,(I,I,I)),
            ("cairo_create",P,(P,)),("cairo_destroy",None,(P,)),("cairo_surface_destroy",None,(P,)),
            ("cairo_surface_flush",None,(P,)),("cairo_image_surface_get_data",C.POINTER(C.c_ubyte),(P,)),
            ("cairo_image_surface_get_stride",I,(P,)),("cairo_surface_write_to_png",I,(P,B))):_bind(c,name,result,*args)
        _bind(pc,"pango_cairo_create_layout",P,P); _bind(pc,"pango_cairo_show_layout",None,P,P)
        _bind(p,"pango_layout_set_text",None,P,B,I); _bind(p,"pango_layout_set_width",None,P,I)
        _bind(p,"pango_layout_set_ellipsize",None,P,I)

    def _signal(self, obj, name, callback):
        cb = C.CFUNCTYPE(None,C.c_void_p,C.c_void_p)(lambda _obj,_data: callback())
        self._callbacks.append(cb); self.gobject.g_signal_connect_data(obj,name.encode(),C.cast(cb,C.c_void_p),None,None,0)

    def _event_signal(self, obj, name, callback):
        cb = C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_void_p)(lambda widget,event,_data: callback(widget,event))
        self._callbacks.append(cb); self.gobject.g_signal_connect_data(obj,name.encode(),C.cast(cb,C.c_void_p),None,None,0)

    def _button(self, box, key, callback):
        b=self.gtk.gtk_button_new_with_label(self.text[key].encode());self._signal(b,"clicked",callback)
        self.gtk.gtk_box_pack_start(box,b,False,False,0);return b

    def _style(self):
        css=b'#hop-layout-title {font-size:24px;font-weight:700;color:#2548c4;} #hop-layout-canvas {background:#f1f4fa;border:1px solid #d9e1ef;border-radius:8px;}'
        provider=self.gtk.gtk_css_provider_new()
        if self.gtk.gtk_css_provider_load_from_data(provider,css,len(css),None):
            self.gtk.gtk_style_context_add_provider_for_screen(self.gdk.gdk_screen_get_default(),provider,600)
        self.gobject.g_object_unref(provider)

    def _changed(self):
        idx=self.gtk.gtk_combo_box_get_active(self.combo)
        if 0 <= idx < len(self.editor.layout.devices): self.editor.select(self.editor.layout.devices[idx].id)
        self._refresh()

    def _toggle(self): self.editor.toggle(); self._refresh()
    def _save(self):
        try: self.editor.save(); self.gtk.gtk_label_set_text(self.note,self.text["saved"].encode())
        except (OSError,ValueError,RuntimeError): self.gtk.gtk_label_set_text(self.note,self.text["bad_save"].encode())

    def _refresh(self):
        result=self.editor.layout.validate()
        status="overlap" if result.errors else "disconnected" if result.warnings else "connected"
        self.gtk.gtk_label_set_text(self.status,self.text[status].encode())
        self.gtk.gtk_label_set_text(self.note,(self.text["unsaved"] if not self.editor.layout_store else "").encode())
        self.gtk.gtk_widget_set_sensitive(self.save_button,bool(self.editor.layout_store and not result.errors))
        self.gtk.gtk_widget_queue_draw(self.canvas)

    def _transform(self):
        if self._drag: return self._drag["transform"]
        boxes=[d.bounds() for d in self.editor.layout.devices]
        if not boxes:return (1.,0.,0.)
        left=min(b[0] for b in boxes);top=min(b[1] for b in boxes)
        right=max(b[0]+b[2] for b in boxes);bottom=max(b[1]+b[3] for b in boxes)
        w=max(1,self.gtk.gtk_widget_get_allocated_width(self.canvas));h=max(1,self.gtk.gtk_widget_get_allocated_height(self.canvas))
        scale=min((w-50)/max(1,right-left),(h-50)/max(1,bottom-top),2.0);scale=max(.001,scale)
        return (scale,25-left*scale,25-top*scale)

    def _text(self,cr,x,y,width,value):
        layout=self.pangocairo.pango_cairo_create_layout(cr)
        try:
            self.pango.pango_layout_set_text(layout,value.encode(),-1);self.pango.pango_layout_set_width(layout,max(1,int(width*1024)))
            self.pango.pango_layout_set_ellipsize(layout,3);self.cairo.cairo_move_to(cr,x,y);self.pangocairo.pango_cairo_show_layout(cr,layout)
        finally:self.gobject.g_object_unref(layout)

    def _draw(self,_widget,context):
        scale,ox,oy=self._transform(); c=self.cairo
        c.cairo_set_source_rgb(context,.945,.958,.982);c.cairo_paint(context)
        invalid=bool(self.editor.layout.validate().errors)
        for d in self.editor.layout.devices:
            x,y,w,h=d.bounds();px,py,pw,ph=ox+x*scale,oy+y*scale,w*scale,h*scale
            c.cairo_set_source_rgb(context,1,1,1);c.cairo_rectangle(context,px,py,pw,ph);c.cairo_fill(context)
            c.cairo_set_source_rgb(context,.8,.23,.19) if invalid else c.cairo_set_source_rgb(context,.25,.38,.82) if d.id==self.editor.selected else c.cairo_set_source_rgb(context,.58,.63,.72)
            c.cairo_set_line_width(context,2);c.cairo_rectangle(context,px,py,pw,ph);c.cairo_stroke(context)
            c.cairo_set_source_rgb(context,.13,.17,.24);self._text(context,px+8,py+6,pw-16,d.name)
            if d.id in self.editor.expanded:
                for s in d.screens:
                    x,y,w,h=d.rectangle(s);sx,sy,sw,sh=ox+x*scale,oy+y*scale,w*scale,h*scale
                    c.cairo_set_source_rgb(context,.53,.62,.78);c.cairo_set_line_width(context,1)
                    c.cairo_rectangle(context,sx+3,sy+3,max(1,sw-6),max(1,sh-6));c.cairo_stroke(context)
                    c.cairo_set_source_rgb(context,.26,.31,.39);self._text(context,sx+8,sy+27,sw-16,s.id)
        return 0

    def _press(self,_widget,event):
        e=C.cast(event,C.POINTER(_ButtonEvent)).contents
        if e.button!=1:return 0
        scale,ox,oy=self._transform(); wx=(e.x-ox)/scale;wy=(e.y-oy)/scale
        for d in reversed(self.editor.layout.devices):
            x,y,w,h=d.bounds()
            if x<=wx<x+w and y<=wy<y+h:
                screen=None
                if d.id in self.editor.expanded:
                    screen=next((s for s in d.screens if (lambda b:b[0]<=wx<b[0]+b[2] and b[1]<=wy<b[1]+b[3])(d.rectangle(s))),None)
                self.editor.start_drag(d.id,screen.id if screen else None)
                ix=d.x+(screen.x if screen else 0);iy=d.y+(screen.y if screen else 0)
                self._drag={"device":d.id,"screen":screen.id if screen else None,"anchor":(wx-ix,wy-iy),"transform":(scale,ox,oy),"origin":(d.x,d.y)}
                idx=next(i for i,item in enumerate(self.editor.layout.devices) if item.id==d.id)
                self.gtk.gtk_combo_box_set_active(self.combo,idx);self._refresh();return 1
        return 0

    def _motion(self,_widget,event):
        if not self._drag:return 0
        e=C.cast(event,C.POINTER(_MotionEvent)).contents;s,ox,oy=self._drag["transform"]
        wx=(e.x-ox)/s-self._drag["anchor"][0];wy=(e.y-oy)/s-self._drag["anchor"][1]
        if self._drag["screen"]:wx-=self._drag["origin"][0];wy-=self._drag["origin"][1]
        try:self.editor.drag_to(round(wx),round(wy));self._refresh()
        except ValueError:pass
        return 1

    def _release(self,_widget,event):
        if not self._drag:return 0
        e=C.cast(event,C.POINTER(_ButtonEvent)).contents
        if e.button!=1:return 0
        self.editor.end_drag();self._drag=None;self._refresh();return 1

    def _settings(self):
        if not self.editor.selected:return
        profile=self.editor.profile(self.editor.selected);g=self.gtk
        dialog=g.gtk_dialog_new();g.gtk_window_set_title(dialog,self.text["prefs"].encode());g.gtk_window_set_transient_for(dialog,self.window)
        g.gtk_dialog_add_button(dialog,self.text["cancel"].encode(),-6);g.gtk_dialog_add_button(dialog,self.text["apply"].encode(),-5)
        box=g.gtk_dialog_get_content_area(dialog);g.gtk_container_set_border_width(box,16)
        controls=[];choices=[]
        for key,value in (("movement",profile.movement),("hscroll",profile.horizontal_scroll),("vscroll",profile.vertical_scroll)):
            row=g.gtk_box_new(0,12);label=g.gtk_label_new(self.text[key].encode());g.gtk_label_set_xalign(label,0)
            multipliers=[.25,.5,.75,1,1.25,1.5,2,3,4]
            if value not in multipliers:multipliers.append(value)
            combo=g.gtk_combo_box_text_new()
            for rate in multipliers:g.gtk_combo_box_text_append_text(combo,(str(rate)+'×').encode())
            g.gtk_combo_box_set_active(combo,multipliers.index(value))
            g.gtk_box_pack_start(row,label,True,True,0);g.gtk_box_pack_start(row,combo,False,False,0);g.gtk_box_pack_start(box,row,False,False,0);controls.append(combo);choices.append(multipliers)
        row=g.gtk_box_new(0,12);label=g.gtk_label_new(self.text['frequency'].encode());g.gtk_label_set_xalign(label,0)
        rates=(60,120,240,500,1000);frequency=g.gtk_combo_box_text_new()
        for rate in rates:g.gtk_combo_box_text_append_text(frequency,str(rate).encode())
        g.gtk_combo_box_set_active(frequency,rates.index(profile.forward_hz))
        g.gtk_box_pack_start(row,label,True,True,0);g.gtk_box_pack_start(row,frequency,False,False,0);g.gtk_box_pack_start(box,row,False,False,0)
        checks=[]
        for key,value in (("reverse_x",profile.reverse_horizontal),("reverse_y",profile.reverse_vertical)):
            button=g.gtk_check_button_new_with_label(self.text[key].encode());g.gtk_toggle_button_set_active(button,value)
            g.gtk_box_pack_start(box,button,False,False,0);checks.append(button)
        hint=g.gtk_label_new(self.text["dpi"].encode());g.gtk_box_pack_start(box,hint,False,False,0)
        g.gtk_widget_show_all(dialog)
        if g.gtk_dialog_run(dialog)==-5:
            updated=PointerProfile(*(options[g.gtk_combo_box_get_active(widget)] for options,widget in zip(choices,controls)),*(bool(g.gtk_toggle_button_get_active(w)) for w in checks),rates[g.gtk_combo_box_get_active(frequency)])
            try:self.editor.set_profile(self.editor.selected,updated)
            except (OSError,ValueError):g.gtk_label_set_text(self.note,self.text["bad_save"].encode())
        g.gtk_widget_destroy(dialog)

    def _quit(self):
        if self.gtk.gtk_main_level():self.gtk.gtk_main_quit()
    def show(self): self.gtk.gtk_widget_show_all(self.window)
    def iterate(self):
        while self.gtk.gtk_events_pending():self.gtk.gtk_main_iteration_do(False)
    def run(self):self.show();self.gtk.gtk_main()
    def close(self):
        if self.window:self.gtk.gtk_widget_destroy(self.window);self.window=None

    def save_virtual_screenshot(self,path):
        if os.environ.get("HOP_VIRTUAL_X_SERVER")!="1":raise RuntimeError("virtual X server required")
        end=time.monotonic()+.5
        while time.monotonic()<end:self.iterate();time.sleep(.01)
        allocation=_Allocation();self.gtk.gtk_widget_get_allocation(self.window,C.byref(allocation));self.gtk.gtk_widget_size_allocate(self.window,C.byref(allocation))
        w=self.gtk.gtk_widget_get_allocated_width(self.window);h=self.gtk.gtk_widget_get_allocated_height(self.window)
        surface=self.cairo.cairo_image_surface_create(0,w,h);context=self.cairo.cairo_create(surface)
        try:
            self.gtk.gtk_widget_draw(self.window,context);self.cairo.cairo_surface_flush(surface)
            pixels=self.cairo.cairo_image_surface_get_data(surface);stride=self.cairo.cairo_image_surface_get_stride(surface)
            colors={(pixels[y*stride+x*4],pixels[y*stride+x*4+1],pixels[y*stride+x*4+2]) for y in range(0,h,max(1,h//40)) for x in range(0,w,max(1,w//40))}
            if len(colors)<4:raise RuntimeError("layout preview was not painted")
            if self.cairo.cairo_surface_write_to_png(surface,os.fsencode(path))!=0:raise RuntimeError("failed to save preview")
        finally:self.cairo.cairo_destroy(context);self.cairo.cairo_surface_destroy(surface)
