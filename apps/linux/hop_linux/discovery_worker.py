"""Private Avahi GLib-loop subprocess, started only by explicit UI action."""
import json
import sys
from dataclasses import asdict


def send(message):
    sys.stdout.write(json.dumps(message,ensure_ascii=False,separators=(',',':'))+'\n');sys.stdout.flush()

def main():
    from gi.repository import GLib
    from .discovery import AvahiDiscovery
    config=json.loads(sys.stdin.buffer.readline(4097))
    discovery=AvahiDiscovery();loop=GLib.MainLoop()
    def changed(hints):send({'type':'hints','items':[asdict(h) for h in hints]})
    try:
        discovery.start(name=config['name'],port=config.get('port'),explicitly_started=True,on_change=changed)
        send({'type':'ready'})
        def input_ready(_source,condition):
            if condition & (GLib.IO_HUP|GLib.IO_ERR):loop.quit();return False
            command=sys.stdin.buffer.readline(64)
            if command==b'stop\n' or not command:loop.quit();return False
            return True
        GLib.io_add_watch(sys.stdin.fileno(),GLib.IO_IN|GLib.IO_HUP|GLib.IO_ERR,input_ready)
        loop.run()
    finally:discovery.close()

if __name__=='__main__':
    try:main()
    except BaseException:
        try:send({'type':'error','message':'Avahi discovery unavailable'})
        except Exception:pass
        sys.exit(1)
