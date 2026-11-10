"""Explicit X11 receiver sink for a negotiated, authenticated v2 session.

Screen geometry must be supplied by a trusted local enumerator; this module
never discovers displays or opens a connection by itself. Shortcut conversion,
held-state entry and high-resolution scrolling are not implemented.
"""

class X11Sink:
    def __init__(self, backend, keymap, screens):
        self.backend, self.keymap = backend, keymap
        self.screens = {}
        for screen in screens:
            sid,x,y,w,h=screen
            if (type(sid) is not int or sid<=0 or sid in self.screens or
                any(type(v) is not int for v in (x,y,w,h)) or min(x,y)<0 or min(w,h)<1 or
                x+w-1>32767 or y+h-1>32767): raise ValueError('unsupported local screen geometry')
            self.screens[sid]=(x,y,w,h)
        if not self.screens: raise ValueError('screen geometry required')
        self.active=None;self.wheel_x=0;self.wheel_y=0

    def _position(self, fields):
        if fields['screen_id'] != self.active: raise ValueError('not the active screen')
        x,y,w,h=self.screens[self.active]
        return (x+(fields['x']*(w-1)+32767)//65535, y+(fields['y']*(h-1)+32767)//65535)

    def enter(self, fields):
        if fields['modifiers'] or fields['buttons'] or fields['screen_id'] not in self.screens:
            raise ValueError('unsupported focus entry state')
        if not self.backend.input_state_clear():raise ValueError('local keys or buttons are held')
        self.active=fields['screen_id'];self.wheel_x=self.wheel_y=0
        self.backend.motion(*self._position(fields))

    def inject(self, event, fields):
        if self.active is None: raise ValueError('no active screen')
        if event=='move': self.backend.motion(*self._position(fields)); return
        if event=='button':
            self.backend.motion(*self._position(fields))
            self.backend.button({1:1,2:3,3:2,4:8,5:9}[fields['button']],bool(fields['down'])); return
        if event=='key':
            # USB keyboard usages map through actual XKB physical names.
            # The modifier mask is metadata; it never causes extra injections.
            self.backend.key_hid(self.keymap, fields['usage'], bool(fields['down'])); return
        if event=='wheel':
            self.wheel_x+=fields['horizontal'];self.wheel_y+=fields['vertical']
            # Truncate toward zero so opposite sub-notch deltas cancel naturally.
            sx=int(self.wheel_x/120);sy=int(self.wheel_y/120)
            self.wheel_x-=sx*120;self.wheel_y-=sy*120
            while sx or sy:
                x=max(-100,min(100,sx));y=max(-100,min(100,sy))
                self.backend.scroll(x,y);sx-=x;sy-=y
            return
        raise ValueError('unsupported input')

    def release_all(self):
        failures=self.backend.release_all();self.active=None;self.wheel_x=self.wheel_y=0
        if failures: raise RuntimeError('failed to release injected X11 input')

    def restore_local(self):
        # No capture is activated by the receiver. A host-specific adapter must
        # ungrab capture here; do not reuse this as a host sink without one.
        pass
