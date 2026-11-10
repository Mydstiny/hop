"""Controller for a local, explicitly labelled sample-layout editor."""
from __future__ import annotations
from .layout import Layout, LayoutStore
from .profiles import PointerProfile, ProfileStore


class LayoutEditor:
    def __init__(self, layout, layout_store=None, profile_store=None):
        if not isinstance(layout, Layout): raise ValueError("layout required")
        if layout.validate().errors: raise ValueError("initial layout overlaps")
        if layout_store is not None and not isinstance(layout_store, LayoutStore): raise ValueError("invalid layout store")
        if profile_store is not None and not isinstance(profile_store, ProfileStore): raise ValueError("invalid profile store")
        self.layout = layout
        self.layout_store = layout_store
        self.profile_store = profile_store
        self.selected = layout.devices[0].id if layout.devices else None
        self.expanded = set()
        self._drag = None
        self._profiles = {}

    def select(self, identity):
        if not any(d.id == identity for d in self.layout.devices): raise ValueError("unknown device")
        self.selected = identity

    def toggle(self):
        if self.selected in self.expanded: self.expanded.remove(self.selected)
        elif self.selected: self.expanded.add(self.selected)

    def profile(self, identity):
        self.select(identity)
        if identity in self._profiles: return self._profiles[identity]
        return self.profile_store.get(identity) if self.profile_store else PointerProfile()

    def set_profile(self, identity, profile):
        self.select(identity)
        if not isinstance(profile, PointerProfile): raise ValueError("invalid profile")
        if self.profile_store: self.profile_store.update(identity, profile)
        self._profiles[identity] = profile

    def _device(self, identity): return next(d for d in self.layout.devices if d.id == identity)

    def start_drag(self, identity, screen_id=None):
        self.select(identity)
        device = self._device(identity)
        if screen_id is not None and (identity not in self.expanded or not any(s.id == screen_id for s in device.screens)):
            raise ValueError("screen is not expanded")
        self._drag = (self.layout, identity, screen_id)

    def _snap(self, original, identity, screen_id, x, y, threshold=16):
        device = next(d for d in original.devices if d.id == identity)
        if screen_id is None:
            moved = [(x + s.x, y + s.y, s.width, s.height) for s in device.screens]
            others = [d.rectangle(s) for d in original.devices if d.id != identity for s in d.screens]
        else:
            s = next(s for s in device.screens if s.id == screen_id)
            moved = [(device.x + x, device.y + y, s.width, s.height)]
            others = [d.rectangle(t) for d in original.devices for t in d.screens if not (d.id == identity and t.id == screen_id)]
        dxs, dys = [], []
        for a in moved:
            for b in others:
                if max(a[1], b[1]) < min(a[1] + a[3], b[1] + b[3]):
                    dxs.extend((b[0] - (a[0] + a[2]), b[0] + b[2] - a[0]))
                if max(a[0], b[0]) < min(a[0] + a[2], b[0] + b[2]):
                    dys.extend((b[1] - (a[1] + a[3]), b[1] + b[3] - a[1]))
        close_x = [v for v in dxs if abs(v) <= threshold]
        close_y = [v for v in dys if abs(v) <= threshold]
        if close_x: x += min(close_x, key=lambda v: (abs(v), v))
        if close_y: y += min(close_y, key=lambda v: (abs(v), v))
        return x, y

    def drag_to(self, x, y, snap=True):
        if not self._drag: raise RuntimeError("no drag")
        original, identity, screen_id = self._drag
        if type(x) is not int or type(y) is not int: raise ValueError("positions must be integers")
        if snap: x, y = self._snap(original, identity, screen_id, x, y)
        self.layout = original.move(identity, x, y) if screen_id is None else original.move_screen(identity, screen_id, x, y)
        return self.layout.validate()

    def end_drag(self):
        if not self._drag: return False
        original, _, _ = self._drag
        self._drag = None
        if self.layout.validate().errors:
            self.layout = original
            return False
        return True

    def save(self):
        if not self.layout_store: raise RuntimeError("no explicit layout file configured")
        self.layout_store.save(self.layout)
