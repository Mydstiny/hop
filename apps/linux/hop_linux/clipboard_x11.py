"""X11 CLIPBOARD adapter; run this component in a dedicated helper process.

Explicitly activated only; no display or clipboard is read at import. This
module does not provide encrypted transport, focus acquisition, owner-change
watching, or a user-facing application. Xlib error handlers are process-global,
so the native driver must run in its own process, not beside a GUI toolkit.
"""
from __future__ import annotations
import ctypes as C
import ctypes.util
import os
import threading
import time

from .clipboard import CHUNK, SelectionOwner, SelectionReceiver, TEXT, PNG, validate_payload

P, W, I, U, L = C.c_void_p, C.c_ulong, C.c_int, C.c_uint, C.c_long
_SELECTION_CLEAR, _SELECTION_REQUEST, _SELECTION_NOTIFY, _PROPERTY_NOTIFY = 29, 30, 31, 28
_PROPERTY_NEW_VALUE, _PROPERTY_DELETE, _PROPERTY_CHANGE_MASK = 0, 1, 1 << 22


class _Selection(C.Structure):
    _fields_ = [("type", I), ("serial", W), ("send_event", I), ("display", P),
                ("requestor", W), ("selection", W), ("target", W), ("property", W), ("time", W)]


class _SelectionRequest(C.Structure):
    _fields_ = [("type", I), ("serial", W), ("send_event", I), ("display", P),
                ("owner", W), ("requestor", W), ("selection", W), ("target", W),
                ("property", W), ("time", W)]


class _SelectionClear(C.Structure):
    _fields_ = [("type", I), ("serial", W), ("send_event", I), ("display", P),
                ("window", W), ("selection", W), ("time", W)]


class _Property(C.Structure):
    _fields_ = [("type", I), ("serial", W), ("send_event", I), ("display", P),
                ("window", W), ("atom", W), ("time", W), ("state", I)]

class _FixesSelection(C.Structure):
    _fields_ = [("type", I), ("serial", W), ("send_event", I), ("display", P),
                ("window", W), ("subtype", I), ("owner", W), ("selection", W),
                ("timestamp", W), ("selection_timestamp", W)]


class _Error(C.Structure):
    _fields_ = [("type", I), ("display", P), ("resourceid", W), ("serial", W),
                ("error_code", C.c_ubyte), ("request_code", C.c_ubyte), ("minor_code", C.c_ubyte)]


class _Event(C.Union):
    _fields_ = [("type", I), ("selection", _Selection), ("request", _SelectionRequest), ("clear", _SelectionClear),
                ("property", _Property), ("fixes", _FixesSelection), ("pad", L * 24)]


def _bind(lib, name, restype, *args):
    fn = getattr(lib, name); fn.restype = restype; fn.argtypes = list(args)
    return fn


class ClipboardX11:
    """One X11 connection/window, on one thread, in a dedicated process.

    The event loop must call pump() frequently and initiate local reads only
    for the currently authorized peer. Changes to focus must call set_focus().
    Each incoming local read gets a distinct requestor window; stale responses
    on a destroyed window cannot be mistaken for a later request. Local data
    are delivered through completed() only after full validation.
    """
    _singleton_lock = threading.Lock()
    _active = False

    def __init__(self, focus, completed, *, exclusive_process=False, watch_changes=False):
        if not exclusive_process:
            raise ValueError("ClipboardX11 requires a dedicated helper process")
        if not callable(completed): raise ValueError("completion callback required")
        if os.environ.get("WAYLAND_DISPLAY") or os.environ.get("XDG_SESSION_TYPE", "").lower() == "wayland":
            raise RuntimeError("Wayland clipboard needs its portal adapter; XWayland is not desktop-wide")
        lib = ctypes.util.find_library("X11")
        if not lib: raise RuntimeError("X11 runtime library unavailable")
        self.x = C.CDLL(lib)
        self._configure()
        self._thread = threading.get_ident()
        self._errors = []
        self._handler_type = C.CFUNCTYPE(I, P, C.POINTER(_Error))
        def error_handler(_display, event):
            self._errors.append(int(event.contents.error_code))
            return 0
        self._handler = self._handler_type(error_handler)
        with ClipboardX11._singleton_lock:
            if ClipboardX11._active: raise RuntimeError("only one X11 clipboard driver is permitted per helper process")
            ClipboardX11._active = True
        self.display = None
        installed = False
        try:
            self._old_handler = self.x.XSetErrorHandler(self._handler); installed = True
            self.display = self.x.XOpenDisplay(None)
            if not self.display: raise RuntimeError("Cannot open X11 display")
            self.owner = SelectionOwner(focus)
            self.receiver = SelectionReceiver(focus)
            self.completed = completed
            self.read_window = 0
            self.read_token = None
            self.read_stage = None;self.read_preferences=()
            self.root = self.x.XDefaultRootWindow(self.display)
            self.window = self._window()
            self.atoms = {name: self.x.XInternAtom(self.display, name.encode(), False)
                          for name in ("CLIPBOARD", "TARGETS", TEXT, PNG, "INCR", "ATOM", "_HOP_SELECTION")}
            if not self.window or not all(self.atoms.values()):
                raise RuntimeError("Cannot allocate X11 clipboard resources")
            self.version=0;self._fixes_event=None;self._fixes=None
            if watch_changes:
                fixes=ctypes.util.find_library('Xfixes')
                if not fixes:raise RuntimeError('XFixes is required for clipboard owner monitoring')
                self._fixes=C.CDLL(fixes)
                _bind(self._fixes,'XFixesQueryExtension',I,P,C.POINTER(I),C.POINTER(I))
                _bind(self._fixes,'XFixesSelectSelectionInput',None,P,W,W,W)
                base,error=I(),I()
                if not self._fixes.XFixesQueryExtension(self.display,C.byref(base),C.byref(error)):
                    raise RuntimeError('X server does not provide XFixes')
                self._fixes_event=base.value
                self._fixes.XFixesSelectSelectionInput(self.display,self.window,self.atoms['CLIPBOARD'],7)
            self._sync()
        except BaseException:
            if self.display: self.x.XCloseDisplay(self.display); self.display = None
            if installed: self.x.XSetErrorHandler(self._old_handler)
            with ClipboardX11._singleton_lock: ClipboardX11._active = False
            raise

    def _configure(self):
        x = self.x
        _bind(x, "XOpenDisplay", P, C.c_char_p)
        _bind(x, "XCloseDisplay", I, P)
        _bind(x, "XDefaultRootWindow", W, P)
        _bind(x, "XCreateSimpleWindow", W, P, W, I, I, U, U, U, W, W)
        _bind(x, "XDestroyWindow", I, P, W)
        _bind(x, "XInternAtom", W, P, C.c_char_p, I)
        _bind(x, "XGetAtomName", P, P, W)
        _bind(x, "XSetSelectionOwner", I, P, W, W, W)
        _bind(x, "XGetSelectionOwner", W, P, W)
        _bind(x, "XConvertSelection", I, P, W, W, W, W, W)
        _bind(x, "XSelectInput", I, P, W, L)
        _bind(x, "XChangeProperty", I, P, W, W, W, I, I, C.POINTER(C.c_ubyte), I)
        _bind(x, "XDeleteProperty", I, P, W, W)
        _bind(x, "XGetWindowProperty", I, P, W, W, L, L, I, W, C.POINTER(W), C.POINTER(I),
              C.POINTER(W), C.POINTER(W), C.POINTER(C.POINTER(C.c_ubyte)))
        _bind(x, "XSendEvent", I, P, W, I, L, C.POINTER(_Event))
        _bind(x, "XPending", I, P)
        _bind(x, "XNextEvent", I, P, C.POINTER(_Event))
        _bind(x, "XPutBackEvent", I, P, C.POINTER(_Event))
        _bind(x, "XFlush", I, P)
        _bind(x, "XSync", I, P, I)
        _bind(x, "XGrabServer", I, P)
        _bind(x, "XUngrabServer", I, P)
        _bind(x, "XFree", I, P)
        # XSetErrorHandler takes and returns a C function pointer.
        _bind(x, "XSetErrorHandler", P, P)

    def _check(self):
        if threading.get_ident() != self._thread: raise RuntimeError("clipboard must run on its owning thread")
        if self.display is None: raise RuntimeError("clipboard is closed")

    def _sync(self):
        self.x.XSync(self.display, False)
        if self._errors:
            errors, self._errors = self._errors, []
            raise RuntimeError("X11 clipboard operation failed with error " + str(errors[0]))

    def _window(self):
        return self.x.XCreateSimpleWindow(self.display, self.root, 0, 0, 1, 1, 0, 0, 0)

    def _atom_name(self, atom):
        return next((name for name, value in self.atoms.items() if value == atom), None)

    def _property_write(self, window, prop, type_name, data):
        if type_name == "ATOM":
            values = (W * len(data))(*(self.atoms[x] for x in data))
            ptr = C.cast(values, C.POINTER(C.c_ubyte)); fmt, count = 32, len(data)
        elif type_name == "INCR":
            values = (W * 1)(data)
            ptr = C.cast(values, C.POINTER(C.c_ubyte)); fmt, count = 32, 1
        else:
            values = (C.c_ubyte * len(data)).from_buffer_copy(data)
            ptr = C.cast(values, C.POINTER(C.c_ubyte)); fmt, count = 8, len(data)
        self.x.XChangeProperty(self.display, window, prop, self.atoms[type_name], fmt, 0, ptr, count)
        self._sync()

    def _property_read(self, window, prop):
        data = bytearray(); offset = 0; current_type = None
        active = self.receiver._state
        if not active: raise RuntimeError("no active clipboard read")
        max_bytes = active["limit"]
        while True:
            actual, fmt, count, after = W(), I(), W(), W()
            ptr = C.POINTER(C.c_ubyte)()
            result = self.x.XGetWindowProperty(self.display, window, prop, offset, CHUNK // 4, False, 0,
                                              C.byref(actual), C.byref(fmt), C.byref(count), C.byref(after), C.byref(ptr))
            try:
                self._sync()
                if result != 0: raise RuntimeError("XGetWindowProperty failed")
                typename = self._atom_name(actual.value)
                if typename == "INCR" and fmt.value == 32 and count.value == 1 and after.value == 0:
                    return ("INCR", 32, C.cast(ptr, C.POINTER(W))[0])
                if typename not in (TEXT, PNG) or fmt.value != 8 or (current_type is not None and current_type != typename):
                    raise ValueError("unsupported clipboard property type")
                current_type = typename
                if len(data) + count.value + after.value > max_bytes:
                    raise ValueError("clipboard property exceeds maximum")
                if count.value: data.extend(C.string_at(ptr, count.value))
                if not after.value: return (typename, 8, bytes(data))
                if not count.value or count.value % 4: raise ValueError("invalid X11 property pagination")
                offset += count.value // 4
            finally:
                if ptr: self.x.XFree(ptr)

    def set_focus(self, focus):
        self._check()
        self.owner.focus = focus; self.receiver.focus = focus
        if self.read_window:
            self._cancel_read()

    def install_remote(self, peer, epoch, target, payload):
        self._check()
        self.owner.install_remote(peer, epoch, target, payload)
        self._claim_selection()

    def _claim_selection(self):
        try:
            self.x.XSetSelectionOwner(self.display, self.atoms["CLIPBOARD"], self.window, 0)
            self._sync()
            if self.x.XGetSelectionOwner(self.display, self.atoms["CLIPBOARD"]) != self.window:
                raise RuntimeError("X11 clipboard ownership denied")
        except BaseException:
            self.owner.selection_lost()
            raise

    def _drain_owner_changes(self):
        end=time.monotonic()+.2; total=0
        while self.x.XPending(self.display):
            total+=self.pump()
            if total>8192 or time.monotonic()>end:raise RuntimeError('clipboard event backlog')

    def local_version(self):
        self._check()
        if self._fixes_event is None:raise RuntimeError('clipboard owner monitoring not enabled')
        self._sync();self._drain_owner_changes()
        return self.version

    def _drain_changes_while_grabbed(self):
        # Do not dispatch application callbacks or selection transfers while
        # the server is globally grabbed. Preserve all unrelated events.
        end=time.monotonic()+.2;deferred=[];count=0
        try:
            while self.x.XPending(self.display):
                event=_Event();self.x.XNextEvent(self.display,C.byref(event));count+=1
                if event.type==self._fixes_event and event.fixes.window==self.window and event.fixes.selection==self.atoms['CLIPBOARD']:
                    self.version+=1
                else:deferred.append(event)
                if count>8192 or time.monotonic()>end:raise RuntimeError('clipboard event backlog')
        finally:
            for event in reversed(deferred):self.x.XPutBackEvent(self.display,C.byref(event))

    def install_remote_if_unchanged(self, peer, epoch, target, payload, expected_version, *, guard=None):
        """Atomically compare owner version and install while the server is grabbed.

        Payload validation happens before the brief grab. XFixes detects owner
        changes; a misbehaving owner that changes contents without announcing
        an ownership update cannot be detected by X11's selection protocol.
        """
        self._check();self.last_install_version=None
        if self._fixes_event is None or type(expected_version) is not int or expected_version<0:
            raise ValueError('owner monitoring and expected version required')
        if guard is not None and not callable(guard):raise ValueError('invalid clipboard guard')
        if not self.owner.focus.permits(peer,epoch):raise PermissionError('inactive clipboard peer')
        validate_payload(target,payload)
        self.x.XGrabServer(self.display)
        try:
            self._sync();self._drain_changes_while_grabbed()
            if self.version!=expected_version or (guard is not None and guard() is not True):return False
            self.owner.target,self.owner.data=target,payload  # validated before taking the server grab
            self._claim_selection()
            self._drain_changes_while_grabbed();self.last_install_version=self.version
            return True
        finally:
            self.x.XUngrabServer(self.display);self._sync()

    def read_local(self, peer, epoch, target):
        return self.read_preferred(peer,epoch,(target,))

    def read_preferred(self,peer,epoch,targets=(PNG,TEXT)):
        """Pick the first advertised allowed format, rejecting file objects."""
        self._check()
        if not isinstance(targets,(tuple,list)) or not targets or len(targets)>2 or any(t not in (TEXT,PNG) for t in targets) or len(set(targets))!=len(targets):
            raise ValueError('invalid clipboard target preference')
        if self.read_window: raise RuntimeError("clipboard read already in progress")
        token = self.receiver.begin(peer, epoch, targets[0])
        owner = self.x.XGetSelectionOwner(self.display, self.atoms["CLIPBOARD"])
        if not owner or owner == self.window:
            self.receiver.cancel(); return False
        self.read_window = self._window(); self.read_token = token; self.read_stage='targets';self.read_preferences=tuple(targets)
        try:
            self.x.XSelectInput(self.display, self.read_window, _PROPERTY_CHANGE_MASK)
            self.x.XConvertSelection(self.display, self.atoms["CLIPBOARD"], self.atoms['TARGETS'],
                                     self.atoms["_HOP_SELECTION"], self.read_window, 0)
            self._sync()
            return True
        except BaseException:
            self._cancel_read(); raise

    def _cancel_read(self):
        self.receiver.cancel(); self.read_token = None; self.read_stage=None;self.read_preferences=()
        if self.read_window:
            self.x.XDestroyWindow(self.display, self.read_window)
            self.read_window = 0
            self.x.XFlush(self.display)

    def cancel_read(self):
        """Cancel an outstanding selection conversion on the owner thread."""
        self._check();self._cancel_read()

    def _notify(self, request, property_atom):
        event = _Event(); event.selection = _Selection(_SELECTION_NOTIFY, 0, True, self.display,
            request.requestor, request.selection, request.target, property_atom, request.time)
        sent = self.x.XSendEvent(self.display, request.requestor, False, 0, C.byref(event))
        self._sync()
        if not sent: raise RuntimeError("X11 selection notification failed")

    def _on_request(self, req):
        if req.selection != self.atoms["CLIPBOARD"] or req.owner != self.window:
            self._notify(req, 0); return
        target = self._atom_name(req.target)
        prop = req.property or req.target
        response = self.owner.request(req.requestor, prop, target)
        if response is None:
            self._notify(req, 0); return
        kind, data = response
        try:
            if kind == "INCR": self.x.XSelectInput(self.display, req.requestor, _PROPERTY_CHANGE_MASK)
            self._property_write(req.requestor, prop, kind, data)
            self._notify(req, prop)
        except (RuntimeError, ValueError):
            self.owner.transfers.pop((req.requestor, prop), None)
            self._notify(req, 0)

    def _read_property(self, initial):
        typename, fmt, value = self._property_read(self.read_window, self.atoms["_HOP_SELECTION"])
        token = self.read_token
        result = self.receiver.initial(token, typename, fmt, value) if initial else self.receiver.chunk(token, typename, fmt, value)
        self.x.XDeleteProperty(self.display, self.read_window, self.atoms["_HOP_SELECTION"])
        self._sync()
        if result is not None:
            target = typename
            self._cancel_read()
            self.completed(target, result)

    @staticmethod
    def _file_target(name):
        normalized=name.lower().replace(' ','')
        if normalized in ('text/uri-list','x-special/gnome-copied-files','application/x-kde-cutselection',
                          'application/x-gtk-file-list','application/vnd.portal.filetransfer'):
            return True
        return normalized.startswith('application/x-qt-windows-mime;') and any(
            value in normalized for value in ('filegroupdescriptor','filename','filecontents','shellidlist'))

    def _read_targets(self):
        actual,fmt,count,after=W(),I(),W(),W();ptr=C.POINTER(C.c_ubyte)()
        result=self.x.XGetWindowProperty(self.display,self.read_window,self.atoms['_HOP_SELECTION'],0,257,False,
                                        self.atoms['ATOM'],C.byref(actual),C.byref(fmt),C.byref(count),C.byref(after),C.byref(ptr))
        try:
            self._sync()
            if result!=0 or actual.value!=self.atoms['ATOM'] or fmt.value!=32 or after.value or not 1<=count.value<=256 or not ptr:
                raise ValueError('invalid clipboard TARGETS')
            atoms=C.cast(ptr,C.POINTER(W));available=set()
            state=self.receiver._state
            if not state:raise RuntimeError('clipboard read no longer active')
            for i in range(count.value):
                atom=atoms[i]
                available.add(atom)
                name_ptr=self.x.XGetAtomName(self.display,atom)
                if not name_ptr:raise ValueError('unknown clipboard target')
                try:
                    name=C.string_at(name_ptr)
                    if len(name)>1024:raise ValueError('clipboard target name too long')
                    if self._file_target(name.decode('utf8','strict')):raise ValueError('file clipboard source is not shared')
                finally:self.x.XFree(name_ptr)
            selected=next((target for target in self.read_preferences if self.atoms[target] in available),None)
            if selected is None:raise ValueError('requested clipboard format unavailable')
            self.receiver.select_target(self.read_token,selected);requested=self.atoms[selected]
            self.x.XDeleteProperty(self.display,self.read_window,self.atoms['_HOP_SELECTION'])
            self.read_stage='data'
            self.x.XConvertSelection(self.display,self.atoms['CLIPBOARD'],requested,self.atoms['_HOP_SELECTION'],self.read_window,0)
            self._sync()
        finally:
            if ptr:self.x.XFree(ptr)

    def pump(self, max_events=128):
        self._check()
        if type(max_events) is not int or not 1 <= max_events <= 1024: raise ValueError("invalid event limit")
        self.owner.expire()
        if self.read_window:
            try: self.receiver._current(self.read_token)
            except RuntimeError: self._cancel_read()
        count = 0
        while count < max_events and self.x.XPending(self.display):
            event = _Event(); self.x.XNextEvent(self.display, C.byref(event)); count += 1
            if self._fixes_event is not None and event.type==self._fixes_event and event.fixes.window==self.window and event.fixes.selection==self.atoms['CLIPBOARD']:
                self.version+=1
            elif event.type == _SELECTION_CLEAR and event.clear.window == self.window and event.clear.selection == self.atoms["CLIPBOARD"]:
                if self.x.XGetSelectionOwner(self.display,self.atoms['CLIPBOARD'])!=self.window:
                    self.owner.selection_lost()
            elif event.type == _SELECTION_REQUEST:
                try:
                    self._on_request(event.request)
                except (RuntimeError, ValueError):
                    # Requestor may disappear while focus is switching. X errors
                    # are scoped to this helper process; do not terminate owner.
                    self.owner.transfers.pop((event.request.requestor, event.request.property or event.request.target), None)
            elif event.type == _SELECTION_NOTIFY and self.read_window and event.selection.requestor == self.read_window:
                state = self.receiver._state
                expected = self.atoms['TARGETS'] if self.read_stage=='targets' else self.atoms[state["target"]] if state else 0
                if (event.selection.selection != self.atoms["CLIPBOARD"] or
                    event.selection.target != expected or
                    event.selection.property != self.atoms["_HOP_SELECTION"]):
                    self._cancel_read()
                else:
                    try:
                        if self.read_stage=='targets':self._read_targets()
                        else:self._read_property(True)
                    except (RuntimeError, ValueError,UnicodeDecodeError): self._cancel_read()
            elif event.type == _PROPERTY_NOTIFY:
                p = event.property
                if self.read_stage=='data' and self.read_window and p.window == self.read_window and p.atom == self.atoms["_HOP_SELECTION"] and p.state == _PROPERTY_NEW_VALUE:
                    state = self.receiver._state
                    if state and state["incr"]:
                        try: self._read_property(False)
                        except (RuntimeError, ValueError): self._cancel_read()
                elif p.state == _PROPERTY_DELETE:
                    item = self.owner.property_deleted(p.window, p.atom)
                    if item:
                        try: self._property_write(p.window, p.atom, *item)
                        except (RuntimeError, ValueError): self.owner.transfers.pop((p.window, p.atom), None)
        return count

    def close(self):
        if self.display is None: return
        self._check()
        try:
            self._cancel_read()
            self.x.XDestroyWindow(self.display, self.window)
        finally:
            self.x.XCloseDisplay(self.display); self.display = None
            self.x.XSetErrorHandler(self._old_handler)
            with ClipboardX11._singleton_lock: ClipboardX11._active = False
