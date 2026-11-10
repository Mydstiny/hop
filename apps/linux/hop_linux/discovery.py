"""Explicit Avahi DNS-SD discovery; all returned data are untrusted hints.

Run the Avahi D-Bus client on a dedicated GLib worker/main loop, not a GTK
input callback. Discovery never verifies certificates or creates trust.
"""
from dataclasses import dataclass
import ipaddress
import socket
import time
from .pair_offer import _name

SERVICE='_hop2._tcp'
TXT=(b'v=2',)
MAX_HINTS=64
_AVAHI_IPV4=0

class DiscoveryError(RuntimeError):pass

@dataclass(frozen=True)
class DeviceHint:
    name:str
    address:str
    port:int
    interface:int
    # This is neither a certificate nor a device identity.


def validate_hint(name,address,port,interface,txt):
    _name(name)
    if type(port) is not int or not 1<=port<=65535 or type(interface) is not int or interface<=0:
        raise DiscoveryError('invalid discovered endpoint')
    if tuple(bytes(v) for v in txt)!=TXT:raise DiscoveryError('unsupported discovery TXT')
    try:ip=ipaddress.ip_address(address)
    except ValueError as exc:raise DiscoveryError('discovery requires a numeric address') from exc
    if ip.is_unspecified or ip.is_multicast or ip.is_loopback:raise DiscoveryError('invalid LAN endpoint')
    if ip.version==6 and ip.is_link_local and '%' not in str(ip):
        try:address=str(ip)+'%'+socket.if_indextoname(interface)
        except OSError as exc:raise DiscoveryError('unknown link-local interface') from exc
    return DeviceHint(name,address,port,interface)


class AvahiDiscovery:
    def __init__(self, *, bus=None):
        try:
            import dbus
            from dbus.mainloop.glib import DBusGMainLoop
        except ImportError as exc:raise DiscoveryError('Avahi D-Bus runtime unavailable') from exc
        self.dbus=dbus;DBusGMainLoop(set_as_default=True)
        try:self.bus=bus if bus is not None else dbus.SystemBus()
        except Exception as exc:raise DiscoveryError('Avahi system bus unavailable') from exc
        self.server=None;self.group=None;self.browser=None;self.hints={};self.recent={};self.callbacks=[];self.started=False

    def start(self, *, name, port, explicitly_started, on_change):
        if self.started or explicitly_started is not True or not callable(on_change):raise DiscoveryError('explicit discovery start required')
        _name(name)
        if port is not None and (type(port) is not int or not 1<=port<=65535):raise DiscoveryError('invalid service port')
        dbus=self.dbus
        try:
            self.server=dbus.Interface(self.bus.get_object('org.freedesktop.Avahi','/',introspect=False),'org.freedesktop.Avahi.Server')
            if port is not None:
                group_path=self.server.EntryGroupNew(timeout=3)
                self.group=dbus.Interface(self.bus.get_object('org.freedesktop.Avahi',group_path,introspect=False),'org.freedesktop.Avahi.EntryGroup')
                self.group.AddService(dbus.Int32(-1),dbus.Int32(_AVAHI_IPV4),dbus.UInt32(0),name,SERVICE,'','',dbus.UInt16(port),dbus.Array([dbus.ByteArray(b'v=2')],signature='ay'),timeout=3)
                self.group.Commit(timeout=3)
            browser_path=self.server.ServiceBrowserNew(dbus.Int32(-1),dbus.Int32(-1),SERVICE,'',dbus.UInt32(0),timeout=3)
            self.browser=dbus.Interface(self.bus.get_object('org.freedesktop.Avahi',browser_path,introspect=False),'org.freedesktop.Avahi.ServiceBrowser')
            self.callbacks=[self.browser.connect_to_signal('ItemNew',lambda *args:self._new(on_change,*args)),
                            self.browser.connect_to_signal('ItemRemove',lambda *args:self._remove(on_change,*args))]
            self.started=True
        except Exception as exc:
            self.close();raise DiscoveryError('Avahi advertisement or browse failed') from exc

    def _new(self,callback,interface,protocol,name,stype,domain,_flags):
        if not self.started or str(stype)!=SERVICE:return
        key=(int(interface),int(protocol),str(name),str(domain));now=time.monotonic()
        if key in self.recent and now-self.recent[key]<.25:return
        if key not in self.recent and len(self.recent)>=MAX_HINTS*2:return
        self.recent[key]=now
        try:
            values=self.server.ResolveService(int(interface),int(protocol),str(name),SERVICE,str(domain),-1,self.dbus.UInt32(0),timeout=3)
            iface,_proto,rname,rtype,_domain,_host,_aproto,address,port,txt,_flags=values
            if str(rtype)!=SERVICE:return
            hint=validate_hint(str(rname),str(address),int(port),int(iface),txt)
            if key not in self.hints and len(self.hints)>=MAX_HINTS:return
            self.hints[key]=hint;callback(tuple(self.hints.values()))
        except Exception:return

    def _remove(self,callback,interface,protocol,name,stype,domain,_flags):
        key=(int(interface),int(protocol),str(name),str(domain))
        if str(stype)==SERVICE and key in self.hints:
            del self.hints[key];self.recent.pop(key,None);callback(tuple(self.hints.values()))

    def close(self):
        self.started=False
        for item in self.callbacks:
            try:item.remove()
            except Exception:pass
        self.callbacks=[]
        for obj in (self.browser,self.group):
            if obj:
                try:obj.Free(timeout=2)
                except Exception:pass
        self.browser=self.group=None;self.hints.clear();self.recent.clear()
