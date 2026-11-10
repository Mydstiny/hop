"""Experimental GTK host pairing screen; all network actions are explicit."""
import ctypes as C
import ctypes.util
from .panel import GtkPanel,_bind

class GtkPairingWindow(GtkPanel):
    def __init__(self,controller,language='zh', *, quit_on_destroy=True, add_device=None):
        self.controller=controller;self.hints=[];self._timer=None
        super().__init__(language,quit_on_destroy=quit_on_destroy)
        g=self.gtk;p=C.c_void_p;i=C.c_int;b=C.c_char_p
        g.gtk_window_set_default_size(self.window,760,720)
        _bind(g,'gtk_combo_box_text_new',p);_bind(g,'gtk_combo_box_text_append_text',None,p,b)
        _bind(g,'gtk_combo_box_text_remove_all',None,p);_bind(g,'gtk_combo_box_get_active',i,p)
        _bind(g,'gtk_combo_box_set_active',None,p,i);_bind(g,'gtk_label_set_selectable',None,p,i)
        _bind(g,'gtk_widget_set_no_show_all',None,p,i);_bind(g,'gtk_widget_hide',None,p)
        _bind(g,'gtk_entry_new',p);_bind(g,'gtk_entry_get_text',b,p);_bind(g,'gtk_entry_set_text',None,p,b);_bind(g,'gtk_entry_set_placeholder_text',None,p,b)
        _bind(g,'gtk_spin_button_new_with_range',p,C.c_double,C.c_double,C.c_double)
        _bind(g,'gtk_spin_button_set_value',None,p,C.c_double);_bind(g,'gtk_spin_button_get_value_as_int',i,p)
        _bind(g,'gtk_spin_button_get_value',C.c_double,p)
        _bind(g,'gtk_dialog_new',p);_bind(g,'gtk_dialog_add_button',p,p,b,i);_bind(g,'gtk_dialog_get_content_area',p,p)
        _bind(g,'gtk_dialog_run',i,p);_bind(g,'gtk_window_set_transient_for',None,p,p)
        _bind(g,'gtk_dialog_response',None,p,i)
        _bind(g,'gtk_check_button_new_with_label',p,b);_bind(g,'gtk_toggle_button_set_active',None,p,i);_bind(g,'gtk_toggle_button_get_active',i,p)
        self.combo=g.gtk_combo_box_text_new();g.gtk_box_pack_start(self.root,self.combo,False,False,0)
        self._connect(self.combo,'changed',self._selected)
        self.manual_row=g.gtk_box_new(0,8);self.manual_ip=g.gtk_entry_new()
        g.gtk_entry_set_placeholder_text(self.manual_ip,'手动输入 IP 地址'.encode())
        self.manual_port=g.gtk_spin_button_new_with_range(1,65535,1);g.gtk_spin_button_set_value(self.manual_port,24891)
        self.manual_button=g.gtk_button_new_with_label('按地址配对'.encode())
        g.gtk_box_pack_start(self.manual_row,self.manual_ip,True,True,0);g.gtk_box_pack_start(self.manual_row,self.manual_port,False,False,0)
        g.gtk_box_pack_start(self.manual_row,self.manual_button,False,False,0);g.gtk_box_pack_start(self.root,self.manual_row,False,False,0)
        self._connect(self.manual_button,'clicked',self._manual)
        self.trusted_row=g.gtk_box_new(0,8);self.trusted_fingerprint=g.gtk_entry_new()
        g.gtk_entry_set_placeholder_text(self.trusted_fingerprint,'已配对设备的完整 SHA-256 指纹'.encode())
        self.trusted_button=g.gtk_button_new_with_label('连接已配对设备'.encode())
        g.gtk_box_pack_start(self.trusted_row,self.trusted_fingerprint,True,True,0)
        g.gtk_box_pack_start(self.trusted_row,self.trusted_button,False,False,0);g.gtk_box_pack_start(self.root,self.trusted_row,False,False,0)
        self._connect(self.trusted_button,'clicked',self._trusted)
        self.trusted_list_row=g.gtk_box_new(0,8);self.trusted_list=g.gtk_combo_box_text_new()
        self.trusted_list_button=g.gtk_button_new_with_label('读取已配对设备'.encode());self.trusted_peers=[]
        g.gtk_box_pack_start(self.trusted_list_row,self.trusted_list,True,True,0)
        g.gtk_box_pack_start(self.trusted_list_row,self.trusted_list_button,False,False,0);g.gtk_box_pack_start(self.root,self.trusted_list_row,False,False,0)
        self._connect(self.trusted_list_button,'clicked',self._refresh_trusted);self._connect(self.trusted_list,'changed',self._trusted_selected)
        self.fingerprints=g.gtk_label_new(b'');g.gtk_label_set_xalign(self.fingerprints,0);g.gtk_label_set_line_wrap(self.fingerprints,True)
        g.gtk_label_set_selectable(self.fingerprints,True);g.gtk_box_pack_start(self.root,self.fingerprints,False,False,0)
        self.cancel_button=g.gtk_button_new_with_label('取消配对'.encode())
        g.gtk_box_pack_start(self.root,self.cancel_button,False,False,0);self._connect(self.cancel_button,'clicked',self._cancel)
        self.listen_button=g.gtk_button_new_with_label('等待接收端连接'.encode())
        g.gtk_box_pack_start(self.root,self.listen_button,False,False,0);self._connect(self.listen_button,'clicked',self._listen)
        self.profile_button=g.gtk_button_new_with_label('当前设备鼠标设置'.encode());g.gtk_widget_set_sensitive(self.profile_button,False)
        g.gtk_box_pack_start(self.root,self.profile_button,False,False,0);self._connect(self.profile_button,'clicked',self._profile)
        self.layout_button=g.gtk_button_new_with_label('排列已连接设备'.encode());g.gtk_widget_set_sensitive(self.layout_button,False)
        g.gtk_box_pack_start(self.root,self.layout_button,False,False,0);self._connect(self.layout_button,'clicked',self._layout)
        self.edge_button=g.gtk_button_new_with_label('启用边缘穿越（实验）'.encode());g.gtk_widget_set_sensitive(self.edge_button,False)
        g.gtk_box_pack_start(self.root,self.edge_button,False,False,0);self._connect(self.edge_button,'clicked',self._edge)
        self.clipboard_option=None
        if controller.clipboard_library and getattr(controller,'enable_input',True):
            self.clipboard_option=g.gtk_check_button_new_with_label('与此连接同步剪贴板（仅文本与 PNG）'.encode())
            g.gtk_box_pack_start(self.root,self.clipboard_option,False,False,0)
            self.clipboard_label=g.gtk_label_new(b'');g.gtk_label_set_xalign(self.clipboard_label,0);g.gtk_label_set_line_wrap(self.clipboard_label,True)
            g.gtk_box_pack_start(self.root,self.clipboard_label,False,False,0)
            self._connect(self.clipboard_option,'toggled',self._clipboard_changed)
            self._render_clipboard()
        self.add_device_button=None
        if add_device:
            self.add_device_button=g.gtk_button_new_with_label('添加另一台设备'.encode())
            g.gtk_box_pack_start(self.root,self.add_device_button,False,False,0)
            def add():
                try:add_device()
                except Exception:self._set_status('无法添加设备，请检查连接数量和桌面权限。')
            self._connect(self.add_device_button,'clicked',add)
        self._connect(self.widgets['refresh'],'clicked',self._discover)
        self._connect(self.widgets['configure'],'clicked',self._pair)
        self._connect(self.widgets['pause'],'clicked',self._confirm)
        self._connect(self.window,'destroy',self._destroyed)
        g.gtk_widget_set_no_show_all(self.language_button,True);g.gtk_widget_hide(self.language_button)
        g.gtk_button_set_label(self.widgets['refresh'],'查找设备'.encode())
        g.gtk_button_set_label(self.widgets['configure'],'配对所选设备'.encode())
        g.gtk_button_set_label(self.widgets['pause'],'两台设备指纹均已核对'.encode())
        g.gtk_label_set_text(self.widgets['heading'],'配对局域网设备'.encode())
        g.gtk_label_set_text(self.widgets['subtitle'],'设备名称和地址仅供查找，身份以证书指纹为准。'.encode())
        g.gtk_label_set_text(self.widgets['status'],'开发连接预览 · 手动开始查找'.encode())
        g.gtk_label_set_text(self.widgets['devices'],'候选设备'.encode())
        g.gtk_label_set_text(self.widgets['empty'],'尚未查找设备'.encode())
        g.gtk_label_set_text(self.widgets['empty_hint'],'选择候选设备后，在独立可信渠道核对完整指纹。'.encode())
        g.gtk_label_set_text(self.widgets['settings'],'身份验证'.encode())
        g.gtk_label_set_text(self.widgets['settings_hint'],'请分别与对方显示的对应指纹核对；两台设备的指纹通常不同。'.encode())
        g.gtk_label_set_text(self.widgets['note'],'开始控制前请松开键鼠并关闭 Caps Lock；按住拖拽跨越暂不支持。'.encode())
        g.gtk_widget_set_sensitive(self.widgets['refresh'],True)
        glib=ctypes.util.find_library('glib-2.0')
        if not glib:raise RuntimeError('GLib runtime unavailable')
        self.glib=C.CDLL(glib);_bind(self.glib,'g_timeout_add',C.c_uint,C.c_uint,C.c_void_p,p);_bind(self.glib,'g_source_remove',i,C.c_uint)
        self._timer_callback=C.CFUNCTYPE(i,p)(lambda _data:self._tick())
        self._timer=self.glib.g_timeout_add(50,C.cast(self._timer_callback,p),None)

    def _set_status(self,value):self.gtk.gtk_label_set_text(self.widgets['status'],value.encode())
    def _discover(self):
        try:self.controller.start_discovery();self._set_status('正在查找设备…');self.gtk.gtk_widget_set_sensitive(self.widgets['refresh'],False)
        except Exception:self._set_status('无法启动发现服务，请检查 Avahi。')
    def _listen(self):
        try:
            self.controller.start_listening()
            self._set_status('正在等待局域网连接 · TCP '+str(self.controller.listen_port))
            self.gtk.gtk_widget_set_sensitive(self.listen_button,False)
        except Exception:self._set_status('无法开始监听，请检查端口是否已被使用。')
    def _discovery_error(self):
        if self.controller.state=='listening':
            self._set_status('局域网发现不可用。请在接收端输入本机 IP 和端口 '+str(self.controller.listen_port)+'。')
        else:
            self.gtk.gtk_widget_set_sensitive(self.widgets['refresh'],True)
            self._set_status('局域网发现不可用。可输入对端 IP 地址和端口。')
    def _selected(self):
        connected=self.controller.state=='connected'
        self.gtk.gtk_button_set_label(self.widgets['configure'],('开始控制' if connected else '配对所选设备').encode())
        allowed=(bool(self.controller.screens) and bool(self.controller.shortcut_library)) if connected else bool(self.hints) and self.controller.worker is None
        self.gtk.gtk_widget_set_sensitive(self.widgets['configure'],allowed)
        index=self.gtk.gtk_combo_box_get_active(self.combo)
        if not connected and 0<=index<len(self.hints):
            hint=self.hints[index]
            self.gtk.gtk_entry_set_text(self.manual_ip,hint.address.encode());self.gtk.gtk_spin_button_set_value(self.manual_port,hint.port)
    def _pair(self):
        if self.controller.state=='connected':
            try:self.controller.start_control();self._set_status('正在切换到对端…');self.gtk.gtk_widget_set_sensitive(self.widgets['configure'],False)
            except Exception:self._set_status('无法开始控制，请检查屏幕和快捷键运行库。')
            return
        index=self.gtk.gtk_combo_box_get_active(self.combo)
        try:
            if not 0<=index<len(self.hints):return
            self.controller.begin_pairing(self.hints[index]);self._set_status('正在交换候选证书…')
            self.gtk.gtk_widget_set_sensitive(self.widgets['configure'],False)
        except Exception:self._set_status('无法开始配对。')
    def _confirm(self):
        try:self.controller.confirm(True);self._set_status('正在建立加密连接…');self.gtk.gtk_widget_set_sensitive(self.widgets['pause'],False)
        except Exception:self._set_status('配对确认失败。')
    def _manual(self):
        try:
            address=self.gtk.gtk_entry_get_text(self.manual_ip).decode('utf8').strip()
            self.controller.begin_manual_pairing(address,self.gtk.gtk_spin_button_get_value_as_int(self.manual_port))
            self._set_status('正在交换候选证书…');self.gtk.gtk_widget_set_sensitive(self.manual_button,False)
        except Exception:self._set_status('请输入有效的 IP 地址和端口。')
    def _trusted(self):
        try:
            address=self.gtk.gtk_entry_get_text(self.manual_ip).decode('utf8').strip()
            fingerprint=self.gtk.gtk_entry_get_text(self.trusted_fingerprint).decode('ascii').strip()
            self.controller.begin_trusted(address,self.gtk.gtk_spin_button_get_value_as_int(self.manual_port),fingerprint)
            self._set_status('正在连接已配对设备…');self.gtk.gtk_widget_set_sensitive(self.trusted_button,False)
        except Exception:self._set_status('请输入有效的地址、端口和完整指纹。')
    def _refresh_trusted(self):
        try:self.controller.refresh_trusted_peers();self.gtk.gtk_widget_set_sensitive(self.trusted_list_button,False)
        except Exception:self._set_status('无法读取已配对设备。')
    def _trusted_selected(self):
        index=self.gtk.gtk_combo_box_get_active(self.trusted_list)
        if 0<=index<len(self.trusted_peers):
            entry=self.trusted_peers[index];self.gtk.gtk_entry_set_text(self.trusted_fingerprint,entry['pin'].encode())
            hint=entry['hint']
            if hint:
                self.gtk.gtk_entry_set_text(self.manual_ip,hint['address'].encode());self.gtk.gtk_spin_button_set_value(self.manual_port,hint['port'])
    def _cancel(self):
        self.controller.cancel_pairing();self.gtk.gtk_widget_set_sensitive(self.widgets['pause'],False);self._set_status('正在取消配对…')
    def _edge(self):
        try:
            self.controller.enable_edge_crossing(not self.controller.edge_enabled)
            self.gtk.gtk_widget_set_sensitive(self.edge_button,False)
        except Exception:self._set_status('无法更改边缘穿越设置')
    def _clipboard_changed(self):
        allowed=bool(self.gtk.gtk_toggle_button_get_active(self.clipboard_option))
        try:self.controller.set_clipboard_allowed(allowed)
        except Exception:
            self.gtk.gtk_toggle_button_set_active(self.clipboard_option,self.controller.clipboard_allowed)
        self._render_clipboard()
    def _render_clipboard(self):
        if not self.clipboard_option:return
        controller=self.controller
        self.gtk.gtk_widget_set_sensitive(self.clipboard_option,controller.worker is None or controller.clipboard_allowed)
        if not controller.clipboard_allowed:
            message='剪贴板已关闭。重新连接前可启用。'
        else:
            message={'off':'已授权此连接。仅在双方启用且正在控制时同步。',
                     'unavailable':'对端未启用剪贴板同步。',
                     'standby':'剪贴板待命。进入控制后开始同步。',
                     'waiting':'正在等待对端剪贴板准备就绪。',
                     'active':'剪贴板同步中。可随时取消授权。',
                     'paused':'对端已暂停剪贴板同步。'}[controller.clipboard_state]
        self.gtk.gtk_label_set_text(self.clipboard_label,message.encode())
    def _profile(self):
        from .profiles import PointerProfile
        if self.controller.state!='connected' or not self.controller.profile_store:return
        g=self.gtk;profile=self.controller.profile;dialog=g.gtk_dialog_new()
        g.gtk_window_set_title(dialog,'当前设备鼠标设置'.encode());g.gtk_window_set_transient_for(dialog,self.window)
        g.gtk_dialog_add_button(dialog,'取消'.encode(),-6);g.gtk_dialog_add_button(dialog,'保存'.encode(),-5)
        box=g.gtk_dialog_get_content_area(dialog);g.gtk_container_set_border_width(box,16);controls=[];choices=[]
        for label,value in (('鼠标移动倍率',profile.movement),('水平滚轮倍率',profile.horizontal_scroll),('垂直滚轮倍率',profile.vertical_scroll)):
            row=g.gtk_box_new(0,12);title=g.gtk_label_new(label.encode());g.gtk_label_set_xalign(title,0)
            rates=[.25,.5,.75,1,1.25,1.5,2,3,4]
            if value not in rates:rates.append(value)
            combo=g.gtk_combo_box_text_new()
            for rate in rates:g.gtk_combo_box_text_append_text(combo,(str(rate)+'×').encode())
            g.gtk_combo_box_set_active(combo,rates.index(value))
            g.gtk_box_pack_start(row,title,True,True,0);g.gtk_box_pack_start(row,combo,False,False,0);g.gtk_box_pack_start(box,row,False,False,0);controls.append(combo);choices.append(rates)
        rates=(60,120,240,500,1000);row=g.gtk_box_new(0,12);title=g.gtk_label_new('鼠标移动目标发送频率（Hz）'.encode());g.gtk_label_set_xalign(title,0)
        frequency=g.gtk_combo_box_text_new()
        for rate in rates:g.gtk_combo_box_text_append_text(frequency,str(rate).encode())
        g.gtk_combo_box_set_active(frequency,rates.index(profile.forward_hz));controls.append(frequency)
        g.gtk_box_pack_start(row,title,True,True,0);g.gtk_box_pack_start(row,frequency,False,False,0);g.gtk_box_pack_start(box,row,False,False,0)
        checks=[]
        for title,value in (('反转水平滚轮',profile.reverse_horizontal),('反转垂直滚轮',profile.reverse_vertical)):
            check=g.gtk_check_button_new_with_label(title.encode());g.gtk_toggle_button_set_active(check,value);g.gtk_box_pack_start(box,check,False,False,0);checks.append(check)
        note=g.gtk_label_new('灵敏度为软件倍率，不改变鼠标硬件 DPI。'.encode());g.gtk_box_pack_start(box,note,False,False,0)
        self._profile_widgets=(dialog,controls,checks)
        g.gtk_widget_show_all(dialog)
        if g.gtk_dialog_run(dialog)==-5:
            updated=PointerProfile(*(options[g.gtk_combo_box_get_active(widget)] for options,widget in zip(choices,controls[:3])),*(bool(g.gtk_toggle_button_get_active(w)) for w in checks),rates[g.gtk_combo_box_get_active(controls[3])])
            try:self.controller.save_profile(updated);self._set_status('正在保存当前设备设置…')
            except Exception:self._set_status('无法保存设备设置。')
        g.gtk_widget_destroy(dialog)
        self._profile_widgets=None
    def _layout(self):
        if self.controller.state!='connected' or not self.controller.layout:return
        from .editor import LayoutEditor
        from .layout_panel import GtkLayoutWindow
        group=self.controller.host_group
        if group:
            class GroupEditor(LayoutEditor):
                def save(editor_self):
                    group.set_layout(editor_self.layout)
                    if not group.layout_store:raise RuntimeError('layout storage unavailable')
                    group.layout_store.save(editor_self.layout)
            editor=GroupEditor(self.controller.layout,group.layout_store,self.controller.profile_store)
        else:editor=LayoutEditor(self.controller.layout,self.controller.layout_store,self.controller.profile_store)
        GtkLayoutWindow(editor,simulated=False).run()
        try:self.controller.set_layout(editor.layout);self._set_status('设备布局已应用')
        except Exception:self._set_status('设备布局无效')
    def _tick(self):
        try:
            for _ in range(16):
                item=self.controller.poll()
                if item is None:break
                self.controller.accept_event(item);kind=item.get('type')
                if kind=='hints':
                    self.hints=list(item['items']);self.gtk.gtk_combo_box_text_remove_all(self.combo)
                    for hint in self.hints:self.gtk.gtk_combo_box_text_append_text(self.combo,(hint.name+'  ·  '+hint.address).encode())
                    if self.hints:self.gtk.gtk_combo_box_set_active(self.combo,0)
                    self.gtk.gtk_label_set_text(self.widgets['empty'],('发现 '+str(len(self.hints))+' 个候选设备').encode())
                    self._selected()
                elif kind=='compare':
                    text='本机 SHA-256：\n'+item['local']+'\n\n对端 SHA-256：\n'+item['peer']
                    self.gtk.gtk_label_set_text(self.fingerprints,text.encode());self._set_status('请通过可信渠道分别核对本机和对端完整指纹。')
                    self.gtk.gtk_widget_set_sensitive(self.widgets['pause'],True)
                elif kind=='connected':
                    self._set_status('配对成功 · 已建立加密连接')
                    self.gtk.gtk_widget_set_sensitive(self.profile_button,bool(self.controller.profile_store))
                elif kind=='screens':
                    self._set_status('已连接 · 对端有 '+str(item['count'])+' 个显示器');self._selected()
                    self.gtk.gtk_widget_set_sensitive(self.layout_button,bool(self.controller.layout))
                    self.gtk.gtk_widget_set_sensitive(self.edge_button,bool(self.controller.layout and self.controller.shortcut_library))
                elif kind=='controlling':
                    self._set_status('正在控制对端 · 按 Ctrl + Alt + Shift + Esc 返回本机')
                    self.gtk.gtk_widget_set_sensitive(self.profile_button,False);self.gtk.gtk_widget_set_sensitive(self.layout_button,False)
                    self.gtk.gtk_widget_set_sensitive(self.edge_button,False)
                elif kind=='returned_local':
                    self._set_status('已返回本机');self._selected();self.gtk.gtk_widget_set_sensitive(self.layout_button,bool(self.controller.layout))
                    self.gtk.gtk_widget_set_sensitive(self.profile_button,bool(self.controller.profile_store))
                    self.gtk.gtk_widget_set_sensitive(self.edge_button,bool(self.controller.layout and self.controller.shortcut_library))
                elif kind in ('edge_enabled','edge_disabled'):
                    self.gtk.gtk_button_set_label(self.edge_button,('停用边缘穿越' if kind=='edge_enabled' else '启用边缘穿越（实验）').encode())
                    self.gtk.gtk_widget_set_sensitive(self.edge_button,bool(self.controller.state=='connected' and self.controller.screens and self.controller.shortcut_library))
                    self._set_status('边缘穿越已启用' if kind=='edge_enabled' else '边缘穿越已停用')
                elif kind=='trusted_peers':
                    self.trusted_peers=list(item['items']);self.gtk.gtk_combo_box_text_remove_all(self.trusted_list)
                    for entry in self.trusted_peers:
                        hint=entry['hint'];label=(hint['name']+' · '+hint['address']+' · ' if hint else '')+entry['pin']
                        self.gtk.gtk_combo_box_text_append_text(self.trusted_list,label.encode())
                    if self.trusted_peers:self.gtk.gtk_combo_box_set_active(self.trusted_list,0)
                    self.gtk.gtk_widget_set_sensitive(self.trusted_list_button,True)
                    self._set_status('已读取 '+str(len(self.trusted_peers))+' 台已配对设备；地址仅用于查找，连接时核验身份。')
                elif kind=='trusted_peers_error':
                    self.gtk.gtk_widget_set_sensitive(self.trusted_list_button,True);self._set_status('无法读取已配对设备，请检查密钥环。')
                elif kind=='endpoints_error':self._set_status('无法读取或保存设备地址，请手动输入。')
                elif kind=='discovery_error':self._discovery_error()
                elif kind=='profile_saved':self._set_status('当前设备设置已保存')
                elif kind=='profile_error':self._set_status('设备设置保存失败')
                elif kind=='focus_busy':self._set_status('另一台设备正在使用键鼠，请先返回本机。');self._selected()
                elif kind=='layout_changed':self.gtk.gtk_widget_set_sensitive(self.layout_button,bool(self.controller.layout and self.controller.state=='connected'))
                elif kind=='layout_error':self._set_status('无法读取或构建设备布局')
                elif kind=='error':self._set_status(item.get('message','连接失败'))
                elif kind=='canceled':self._set_status('已取消配对')
                elif kind=='stopped':
                    self.gtk.gtk_widget_set_sensitive(self.widgets['pause'],False);self.gtk.gtk_widget_set_sensitive(self.manual_button,True)
                    self.gtk.gtk_widget_set_sensitive(self.profile_button,False)
                    self.gtk.gtk_widget_set_sensitive(self.layout_button,False)
                    self.gtk.gtk_widget_set_sensitive(self.edge_button,False)
                    self.gtk.gtk_button_set_label(self.edge_button,'启用边缘穿越（实验）'.encode())
                    self.gtk.gtk_widget_set_sensitive(self.trusted_button,True);self._selected()
                    if self.controller.discovery is None:self.gtk.gtk_widget_set_sensitive(self.widgets['refresh'],True)
            self._render_clipboard()
            self.gtk.gtk_widget_set_sensitive(self.listen_button,self.controller.worker is None)
        except Exception:self._set_status('界面状态更新失败')
        return 1
    def _destroyed(self):
        if self._timer:self.glib.g_source_remove(self._timer);self._timer=None
        self.controller.close()
        self.window=None


class GtkReceiverPairingWindow(GtkPairingWindow):
    def __init__(self,controller,language='zh'):
        super().__init__(controller,language)
        g=self.gtk
        g.gtk_button_set_label(self.widgets['refresh'],'开始接收配对'.encode())
        g.gtk_label_set_text(self.widgets['heading'],'接收设备配对'.encode())
        g.gtk_label_set_text(self.widgets['empty'],'可等待主机连接，或按地址连接正在等待配对的主机。'.encode())
        g.gtk_label_set_text(self.widgets['note'],'配对后仅建立加密会话；接收输入和剪贴板尚未接入此界面。'.encode())
        if controller.enable_input:
            g.gtk_label_set_text(self.widgets['note'],'已启用实验性 X11 输入接收；进入前请松开键鼠并关闭 Caps Lock。'.encode())
        g.gtk_widget_set_no_show_all(self.combo,True);g.gtk_widget_hide(self.combo)
        g.gtk_widget_set_no_show_all(self.listen_button,True);g.gtk_widget_hide(self.listen_button)
        g.gtk_entry_set_placeholder_text(self.manual_ip,'输入主机 IP 地址'.encode())
        g.gtk_widget_set_no_show_all(self.profile_button,True);g.gtk_widget_hide(self.profile_button)
        g.gtk_widget_set_no_show_all(self.layout_button,True);g.gtk_widget_hide(self.layout_button)
        g.gtk_widget_set_no_show_all(self.edge_button,True);g.gtk_widget_hide(self.edge_button)
        g.gtk_widget_set_no_show_all(self.widgets['configure'],True);g.gtk_widget_hide(self.widgets['configure'])

    def _discover(self):
        try:self.controller.start_discovery();self._set_status('正在等待其他设备…');self.gtk.gtk_widget_set_sensitive(self.widgets['refresh'],False)
        except Exception:self._set_status('无法启动接收服务，请检查 Avahi。')

    def _discovery_error(self):
        self._set_status('局域网发现不可用。请在主机上输入本机 IP 和端口 '+str(self.controller.listen_port)+'。')

    def _selected(self):self.gtk.gtk_widget_set_sensitive(self.widgets['configure'],False)
