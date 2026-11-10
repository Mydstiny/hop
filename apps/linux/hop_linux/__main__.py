"""Hop Linux diagnostics and native configuration preview."""
import argparse
import json
from .diagnostics import inventory

parser = argparse.ArgumentParser(description=__doc__)
group = parser.add_mutually_exclusive_group(required=True)
group.add_argument("--check", action="store_true", help="print a read-only capability inventory")
group.add_argument("--preview-ui", action="store_true", help="open the native GTK3 development preview")
group.add_argument("--layout-preview", action="store_true", help="open the simulated multi-device layout editor")
group.add_argument("--pairing-ui", action="store_true", help="open experimental explicit host discovery and pairing")
group.add_argument("--receive-pairing-ui", action="store_true", help="open experimental explicit receiver pairing")
group.add_argument("--app", action="store_true", help="open the experimental unified role chooser")
parser.add_argument("--tls-library", help="absolute trusted path to the shared TLS runtime (required for pairing UI)")
parser.add_argument("--shortcut-library", help="absolute trusted path to the shared shortcut runtime (required for host control)")
parser.add_argument("--clipboard-library", help="absolute trusted path to the shared clipboard runtime")
parser.add_argument("--device-name", default="Hop Linux", help="local name for experimental discovery")
parser.add_argument("--port", type=int, default=24891, help="host or receiver listen port (v2 default: 24891)")
parser.add_argument("--enable-receiver-input", action="store_true", help="explicitly enable experimental X11 input reception")
args = parser.parse_args()
if args.check:
    print(json.dumps(inventory(), ensure_ascii=False, indent=2))
elif args.preview_ui:
    from .panel import GtkPanel
    GtkPanel().run()
elif args.layout_preview:
    from .editor import LayoutEditor
    from .layout import sample_layout
    from .layout_panel import GtkLayoutWindow
    GtkLayoutWindow(LayoutEditor(sample_layout())).run()
elif args.app:
    if not args.tls_library:parser.error('--app requires --tls-library')
    from .x11 import initialize_xlib_threads
    initialize_xlib_threads()
    from .app_ui import GtkRoleChooser,GtkHostWorkspace,GtkTray,WorkspaceShutdownError
    from .tls import TLSLibrary
    from .secret_service import SecretStore
    from .controller import PairingController,ReceiverPairingController
    from .pair_ui import GtkPairingWindow,GtkReceiverPairingWindow
    from .profiles import ProfileStore
    from .layout import LayoutStore
    from .endpoints import EndpointStore
    from .host_group import HostGroup
    from pathlib import Path
    import os
    notice=None
    x11_available=bool(os.environ.get('DISPLAY')) and not (os.environ.get('WAYLAND_DISPLAY') or os.environ.get('XDG_SESSION_TYPE','').lower()=='wayland')
    while True:
        chooser=GtkRoleChooser(notice,clipboard_available=bool(args.clipboard_library),x11_available=x11_available)
        role=chooser.run();notice=None
        clipboard_library=args.clipboard_library if chooser.clipboard_available else None
        if role=='quit':break
        if role=='layout':
            from .editor import LayoutEditor
            from .layout import sample_layout
            from .layout_panel import GtkLayoutWindow
            GtkLayoutWindow(LayoutEditor(sample_layout())).run();continue
        try:
            library=TLSLibrary(args.tls_library);store=SecretStore(tls_library=library);identity=store.get_identity()
            if identity is None:identity=library.generate_identity();store.store_identity(*identity)
            config_root=Path(os.environ.get('XDG_CONFIG_HOME') or (Path.home()/'.config'))
            if not config_root.is_absolute():raise ValueError('invalid configuration path')
            if role=='host':
                layout_store=LayoutStore(config_root/'hop'/'layout.json');group=HostGroup(layout_store)
                profiles=ProfileStore(config_root/'hop'/'profiles.json');endpoints=EndpointStore(config_root/'hop'/'endpoints.json')
                def make_host():return PairingController(name=args.device_name,certificate=identity[0],private_key=identity[1],tls_library=library,trust_store=store,
                    shortcut_library=args.shortcut_library,clipboard_library=clipboard_library,profile_store=profiles,layout_store=layout_store,endpoint_store=endpoints,host_group=group,listen_port=args.port)
                if GtkHostWorkspace(make_host).run(tray_enabled=x11_available):break
            else:
                controller=ReceiverPairingController(name=args.device_name,certificate=identity[0],private_key=identity[1],tls_library=library,trust_store=store,
                    listen_port=args.port,enable_input=role=='receiver_input',clipboard_library=clipboard_library if role=='receiver_input' else None)
                window=GtkReceiverPairingWindow(controller);tray=None
                try:
                    if x11_available:tray=GtkTray(window)
                    window.run()
                finally:
                    if tray:tray.close()
                    window.close()
                    worker=controller.worker
                    if worker and worker.is_alive():worker.join(4)
                    if worker and worker.is_alive():raise WorkspaceShutdownError('receiver worker did not stop')
                if tray and tray.exit_requested:break
        except WorkspaceShutdownError:
            break
        except Exception:
            notice='无法启动连接：请检查系统密钥环、运行库和桌面权限。'
else:
    if not args.tls_library: parser.error("pairing UI requires --tls-library")
    if args.enable_receiver_input:
        if not args.receive_pairing_ui:parser.error('--enable-receiver-input requires --receive-pairing-ui')
        from .x11 import initialize_xlib_threads
        initialize_xlib_threads()
    from .tls import TLSLibrary
    from .secret_service import SecretStore
    from .controller import PairingController,ReceiverPairingController
    from .pair_ui import GtkPairingWindow,GtkReceiverPairingWindow
    from .profiles import ProfileStore
    from .layout import LayoutStore
    from .endpoints import EndpointStore
    from .host_group import HostGroup
    from .app_ui import GtkHostWorkspace
    from pathlib import Path
    import os
    library=TLSLibrary(args.tls_library)
    store=SecretStore(tls_library=library)
    identity=store.get_identity()
    if identity is None:
        identity=library.generate_identity()
        store.store_identity(*identity)
    controller_type=ReceiverPairingController if args.receive_pairing_ui else PairingController
    window_type=GtkReceiverPairingWindow if args.receive_pairing_ui else GtkPairingWindow
    config_root=Path(os.environ.get('XDG_CONFIG_HOME') or (Path.home()/'.config'))
    if not config_root.is_absolute():raise RuntimeError('XDG_CONFIG_HOME must be absolute')
    profile_store=ProfileStore(config_root/'hop'/'profiles.json')
    kwargs={'enable_input':args.enable_receiver_input,'listen_port':args.port,'clipboard_library':args.clipboard_library} if args.receive_pairing_ui else {'shortcut_library':args.shortcut_library,'clipboard_library':args.clipboard_library,'profile_store':profile_store,'layout_store':LayoutStore(config_root/'hop'/'layout.json'),'endpoint_store':EndpointStore(config_root/'hop'/'endpoints.json'),'listen_port':args.port}
    if args.receive_pairing_ui:
        window_type(controller_type(name=args.device_name,certificate=identity[0],private_key=identity[1],tls_library=library,trust_store=store,**kwargs)).run()
    else:
        group=HostGroup(kwargs['layout_store']);kwargs['host_group']=group
        GtkHostWorkspace(lambda:PairingController(name=args.device_name,certificate=identity[0],private_key=identity[1],tls_library=library,trust_store=store,**kwargs)).run()
