"""Run only under a newly created private D-Bus session, never a system bus."""
import os
import sys
if os.environ.get('HOP_PRIVATE_AVAHI_TEST')!='1':raise SystemExit('private Avahi test required')
import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib
from hop_linux.discovery import AvahiDiscovery,SERVICE
DBusGMainLoop(set_as_default=True)
bus=dbus.SessionBus();name=dbus.service.BusName('org.freedesktop.Avahi',bus) if sys.argv[1:]==['--service'] else None

class Group(dbus.service.Object):
    def __init__(self):super().__init__(bus,'/test/group');self.published=False
    @dbus.service.method('org.freedesktop.Avahi.EntryGroup',in_signature='iiussssqaay',out_signature='')
    def AddService(self,interface,protocol,flags,name,stype,domain,host,port,txt):
        assert int(protocol)==0 and str(stype)==SERVICE and bytes(txt[0])==b'v=2' and int(port)==41337
        self.published=True
    @dbus.service.method('org.freedesktop.Avahi.EntryGroup',in_signature='',out_signature='')
    def Commit(self):assert self.published
    @dbus.service.method('org.freedesktop.Avahi.EntryGroup',in_signature='',out_signature='')
    def Free(self):pass

class Browser(dbus.service.Object):
    def __init__(self):super().__init__(bus,'/test/browser')
    @dbus.service.signal('org.freedesktop.Avahi.ServiceBrowser',signature='iisssu')
    def ItemNew(self,interface,protocol,name,stype,domain,flags):pass
    @dbus.service.signal('org.freedesktop.Avahi.ServiceBrowser',signature='iisssu')
    def ItemRemove(self,interface,protocol,name,stype,domain,flags):pass
    @dbus.service.method('org.freedesktop.Avahi.ServiceBrowser',in_signature='',out_signature='')
    def Free(self):pass

class Server(dbus.service.Object):
    def __init__(self):super().__init__(bus,'/');self.group=Group();self.browser=Browser()
    @dbus.service.method('org.freedesktop.Avahi.Server',in_signature='',out_signature='o')
    def EntryGroupNew(self):return '/test/group'
    @dbus.service.method('org.freedesktop.Avahi.Server',in_signature='iissu',out_signature='o')
    def ServiceBrowserNew(self,interface,protocol,stype,domain,flags):
        assert str(stype)==SERVICE
        GLib.timeout_add(150,lambda:(self.browser.ItemNew(1,0,'candidate',SERVICE,'local',0),False)[1])
        return '/test/browser'
    @dbus.service.method('org.freedesktop.Avahi.Server',in_signature='iisssiu',out_signature='iissssisqaayu')
    def ResolveService(self,interface,protocol,name,stype,domain,aprotocol,flags):
        return (interface,protocol,name,stype,domain,'hint.local',0,'192.168.1.2',41337,[dbus.ByteArray(b'v=2')],0)

# Fake daemon runs on a separate connection/thread/process in the launcher;
# making synchronous D-Bus calls to the same thread would deadlock.
if sys.argv[1:]==['--service']:
    server=Server();print('READY',flush=True);GLib.MainLoop().run()
elif sys.argv[1:]==['--client']:
    # Separate process owns a different bus connection; no live Avahi daemon.
    d=AvahiDiscovery(bus=bus);loop=GLib.MainLoop();results=[]
    def changed(values):results.extend(values);loop.quit()
    d.start(name='test',port=41337,explicitly_started=True,on_change=changed)
    GLib.timeout_add(3000,lambda:(loop.quit(),False)[1]);loop.run();d.close()
    assert len(results)==1 and results[0].address=='192.168.1.2' and results[0].port==41337
    print('PASS: private D-Bus fake Avahi advertisement, browse and untrusted address hint')
elif sys.argv[1:]==['--worker-client']:
    import time
    from hop_linux.discovery_process import DiscoveryProcess
    d=DiscoveryProcess();values=[]
    try:
        d.start(name='test',port=41337,explicitly_started=True);deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            item=d.poll()
            if item and item.get('type')=='error':raise RuntimeError(item.get('message'))
            if item and item.get('type')=='hints':values.extend(item['items']);break
            time.sleep(.01)
        assert len(values)==1 and values[0].address=='192.168.1.2'
    finally:d.close()
    print('PASS: isolated Avahi worker process discovery and explicit teardown')
else:raise SystemExit('explicit fake Avahi role required')
