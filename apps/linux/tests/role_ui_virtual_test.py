import ctypes as C
import ctypes.util
import os

def run():
    if os.environ.get('HOP_VIRTUAL_X_SERVER')!='1':raise RuntimeError('virtual X server required')
    from hop_linux.app_ui import GtkRoleChooser
    glib=C.CDLL(ctypes.util.find_library('glib-2.0'));glib.g_timeout_add.argtypes=[C.c_uint,C.c_void_p,C.c_void_p];glib.g_timeout_add.restype=C.c_uint
    for role in ('host','receiver','receiver_input','layout','quit'):
        chooser=GtkRoleChooser()
        callback=C.CFUNCTYPE(C.c_int,C.c_void_p)(lambda _,role=role:(chooser.g.gtk_button_clicked(chooser.buttons[role]),0)[1])
        glib.g_timeout_add(30,C.cast(callback,C.c_void_p),None)
        assert chooser.run()==role
    print('PASS: unified role chooser requires explicit user action')
