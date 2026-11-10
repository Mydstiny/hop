"""Experimental unified role launcher; nothing connects before user action."""
import ctypes as C
import ctypes.util

class GtkRoleChooser:
    def __init__(self, notice=None, *, clipboard_available=False, x11_available=True):
        name=ctypes.util.find_library('gtk-3');obj=ctypes.util.find_library('gobject-2.0')
        if not name or not obj:raise RuntimeError('GTK3 runtime unavailable')
        self.g=C.CDLL(name);self.o=C.CDLL(obj);p=C.c_void_p;i=C.c_int;b=C.c_char_p
        def bind(lib,n,r,*args):f=getattr(lib,n);f.restype=r;f.argtypes=list(args)
        for n,r,args in (('gtk_init_check',i,(p,p)),('gtk_window_new',p,(i,)),('gtk_window_set_title',None,(p,b)),
            ('gtk_window_set_default_size',None,(p,i,i)),('gtk_container_set_border_width',None,(p,C.c_uint)),('gtk_container_add',None,(p,p)),
            ('gtk_box_new',p,(i,i)),('gtk_box_pack_start',None,(p,p,i,i,C.c_uint)),('gtk_label_new',p,(b,)),
            ('gtk_label_set_xalign',None,(p,C.c_float)),('gtk_label_set_line_wrap',None,(p,i)),('gtk_button_new_with_label',p,(b,)),
            ('gtk_button_clicked',None,(p,)),
            ('gtk_check_button_new_with_label',p,(b,)),('gtk_toggle_button_get_active',i,(p,)),
            ('gtk_toggle_button_set_active',None,(p,i)),('gtk_widget_set_sensitive',None,(p,i)),
            ('gtk_widget_show_all',None,(p,)),('gtk_widget_destroy',None,(p,)),('gtk_main',None,()),('gtk_main_quit',None,()),('gtk_main_level',C.c_uint,())):
            bind(self.g,n,r,*args)
        bind(self.o,'g_signal_connect_data',C.c_ulong,p,b,p,p,p,i)
        if not self.g.gtk_init_check(None,None):raise RuntimeError('GTK cannot open display')
        self.window=self.g.gtk_window_new(0);self.g.gtk_window_set_title(self.window,'Hop · 选择设备角色'.encode())
        self.g.gtk_window_set_default_size(self.window,540,360);self.g.gtk_container_set_border_width(self.window,24)
        root=self.g.gtk_box_new(1,12);self.g.gtk_container_add(self.window,root);self.callbacks=[];self.choice=None
        for text in ('Hop · 设备连接','选择这台设备的角色。连接前需要双方核对证书指纹。',
                     '键鼠控制和剪贴板同步需要 X11 桌面会话。'):
            label=self.g.gtk_label_new(text.encode());self.g.gtk_label_set_xalign(label,0);self.g.gtk_label_set_line_wrap(label,True)
            self.g.gtk_box_pack_start(root,label,False,False,0)
        if notice:
            label=self.g.gtk_label_new(notice.encode());self.g.gtk_label_set_xalign(label,0);self.g.gtk_label_set_line_wrap(label,True)
            self.g.gtk_box_pack_start(root,label,False,False,0)
        self.buttons={}
        self.clipboard_option=None;self.clipboard_available=False
        if clipboard_available and x11_available:
            self.clipboard_option=self.g.gtk_check_button_new_with_label('允许各设备连接启用剪贴板同步'.encode())
            self.g.gtk_toggle_button_set_active(self.clipboard_option,True)
            self.g.gtk_box_pack_start(root,self.clipboard_option,False,False,0)
        for choice,text in (('host','作为键鼠主机'),('receiver','作为接收端（仅配对）'),('receiver_input','作为接收端（启用 X11 输入）'),('layout','查看设备布局示例'),('quit','关闭')):
            button=self.g.gtk_button_new_with_label(text.encode());self.buttons[choice]=button;self.g.gtk_box_pack_start(root,button,False,False,0)
            self._signal(button,'clicked',lambda choice=choice:self._choose(choice))
            if not x11_available and choice in ('host','receiver_input'):self.g.gtk_widget_set_sensitive(button,False)
        self._signal(self.window,'destroy',self._destroyed)
    def _signal(self,obj,name,fn):
        callback=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p)(lambda _object,_data:fn());self.callbacks.append(callback)
        self.o.g_signal_connect_data(obj,name.encode(),C.cast(callback,C.c_void_p),None,None,0)
    def _choose(self,choice):
        if self.choice is None:self.choice=choice
        if self.g.gtk_main_level():self.g.gtk_main_quit()
    def _destroyed(self):
        self.window=None;self._choose('quit')
    def run(self):
        self.g.gtk_widget_show_all(self.window);self.g.gtk_main();choice=self.choice or 'quit'
        if self.window:
            if self.clipboard_option:self.clipboard_available=bool(self.g.gtk_toggle_button_get_active(self.clipboard_option))
            self.g.gtk_widget_destroy(self.window)
        return choice


class WorkspaceShutdownError(RuntimeError):pass


class GtkTray:
    """Optional X11 notification-area icon. Window controls remain available."""
    def __init__(self, primary, *, workspace=None):
        from .panel import _bind
        self.primary=primary;self.workspace=workspace;self.exit_requested=False
        self.gtk=primary.gtk;self.glib=primary.glib
        p=C.c_void_p;b=C.c_char_p;i=C.c_int;u=C.c_uint
        for name,returns,params in (
            ('gtk_status_icon_new_from_icon_name',p,(b,)),('gtk_status_icon_set_title',None,(p,b)),
            ('gtk_status_icon_set_tooltip_text',None,(p,b)),('gtk_status_icon_set_visible',None,(p,i)),
            ('gtk_window_present',None,(p,)),('gtk_menu_new',p,()),('gtk_menu_item_new_with_label',p,(b,)),
            ('gtk_menu_shell_append',None,(p,p)),('gtk_menu_popup',None,(p,p,p,p,p,u,u)),
        ):
            _bind(self.gtk,name,returns,*params)
        self.icon=self.gtk.gtk_status_icon_new_from_icon_name(b'input-mouse')
        self.gtk.gtk_status_icon_set_title(self.icon,b'Hop')
        self.gtk.gtk_status_icon_set_visible(self.icon,True)
        primary._connect(self.icon,'activate',self.present)
        self.popup_callback=C.CFUNCTYPE(None,p,u,u,p)(self._popup)
        primary.gobject.g_signal_connect_data(self.icon,b'popup-menu',C.cast(self.popup_callback,p),None,None,0)
        self.menu=self.gtk.gtk_menu_new()
        self._item('打开 Hop',self.present)
        if workspace:
            self.pause_item=self._item('暂停边缘切换并返回本机',workspace.pause)
            self.resume_item=self._item('恢复边缘穿越',workspace.resume_edges)
        else:
            self.pause_item=self._item('断开连接',primary.controller.cancel_pairing)
            self.resume_item=None
        self._item('退出 Hop',self._quit)
        self.gtk.gtk_widget_show_all(self.menu)
        self._refresh()
        self.timer_callback=C.CFUNCTYPE(i,p)(lambda _data:self._refresh())
        self.timer=self.glib.g_timeout_add(250,C.cast(self.timer_callback,p),None)

    def _item(self,label,action):
        item=self.gtk.gtk_menu_item_new_with_label(label.encode())
        self.primary._connect(item,'activate',action)
        self.gtk.gtk_menu_shell_append(self.menu,item)
        return item

    def _windows(self):
        return self.workspace.windows if self.workspace else [self.primary]

    def _active(self):
        return next((window for window in self._windows() if window.window and window.controller.state=='controlling'),None)

    def _refresh(self):
        active=self._active()
        connected=[w.controller for w in self._windows() if w.window and w.controller.connected_name]
        if active:status='正在控制 '+(active.controller.connected_name or '对端')
        elif connected:status='已连接 '+str(len(connected))+' 台设备'
        else:status='等待连接'
        self.gtk.gtk_status_icon_set_tooltip_text(self.icon,('Hop · '+status).encode())
        if self.workspace:
            group=self.primary.controller.host_group
            edges=group and group.edges_enabled
            self.gtk.gtk_widget_set_sensitive(self.pause_item,bool(active or edges or any(c.state=='entering' for c in connected)))
            self.gtk.gtk_widget_set_sensitive(self.resume_item,bool(group and not edges and any(c.screens for c in connected)))
        else:self.gtk.gtk_widget_set_sensitive(self.pause_item,self.primary.controller.worker is not None)
        return True

    def present(self):
        window=self._active() or self.primary
        if window.window:window.gtk.gtk_window_present(window.window)

    def _popup(self,_icon,button,timestamp,_data):
        self._refresh()
        position=C.cast(self.gtk.gtk_status_icon_position_menu,C.c_void_p)
        self.gtk.gtk_menu_popup(self.menu,None,None,position,self.icon,button,timestamp)

    def _quit(self):
        self.exit_requested=True
        self.primary.close()

    def close(self):
        self.glib.g_source_remove(self.timer)
        self.gtk.gtk_status_icon_set_visible(self.icon,False)
        self.gtk.gtk_widget_destroy(self.menu)
        self.primary.gobject.g_object_unref(self.icon)


class GtkHostWorkspace:
    """Own one GTK window per device and one shared focus authority."""
    def __init__(self,controller_factory):
        if not callable(controller_factory):raise ValueError('controller factory required')
        self.factory=controller_factory;self.windows=[];self.closing=False

    def add_device(self, *, primary=False):
        if self.closing or len(self.windows)>=32:raise RuntimeError('device limit reached')
        from .pair_ui import GtkPairingWindow
        controller=self.factory();window=GtkPairingWindow(controller,quit_on_destroy=primary,add_device=self.add_device)
        self.windows.append(window)
        if not primary:window.show()
        return window

    def pause(self):
        if self.windows:
            group=self.windows[0].controller.host_group
            if group:group.set_edges_enabled(False,pause_handoffs=True)
        for window in self.windows:window.controller.request_local_focus()

    def resume_edges(self):
        if self.windows:
            group=self.windows[0].controller.host_group
            if group:group.set_edges_enabled(True)

    def run(self, *, tray_enabled=False):
        import time
        primary=self.add_device(primary=True)
        tray=None
        try:
            if tray_enabled:tray=GtkTray(primary,workspace=self)
            primary.run()
        finally:
            self.closing=True
            if tray:tray.close()
            for window in self.windows:window.close()
            deadline=time.monotonic()+4
            for window in self.windows:
                worker=window.controller.worker
                if worker and worker.is_alive():worker.join(max(0,deadline-time.monotonic()))
            if any(w.controller.worker and w.controller.worker.is_alive() for w in self.windows):
                raise WorkspaceShutdownError('session worker did not stop')
        return bool(tray and tray.exit_requested)
