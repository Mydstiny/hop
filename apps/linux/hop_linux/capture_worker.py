"""Private capture subprocess. Invoked only by x11_host_process."""
import ctypes as C
import ctypes.util
import json
import os
import select
import sys
import time


def _send(value):
    sys.stdout.write(json.dumps(value,separators=(',',':'))+'\n');sys.stdout.flush()

class _Forward:
    def send_input(self, event, **fields): _send(dict(type='input',event=event,fields=fields))
    def close(self): pass


def main():
    # Fresh interpreter: initialize Xlib thread support before importing GTK or
    # opening any Xlib connection, regardless of the parent's initialization.
    lib=C.CDLL(ctypes.util.find_library('X11'));lib.XInitThreads.restype=C.c_int
    if not lib.XInitThreads(): raise RuntimeError('XInitThreads failed')
    from .x11_host import X11Host
    from .profiles import PointerProfile
    line=sys.stdin.buffer.readline(8193)
    if len(line)>8192 or not line.endswith(b'\n'): raise ValueError('invalid worker configuration')
    config=json.loads(line)
    profile=PointerProfile(**config['profile'])
    host=X11Host(screen_id=config['screen_id'],target_size=tuple(config['target_size']),
                 capture_center=tuple(config['capture_center']),initial=tuple(config['initial']),profile=profile,
                 edge_directions=tuple(config.get('edge_directions',())))
    try:
        host.start(acknowledged=True,input_state_clear=True)
        _send(dict(type='ready'));last_tick=time.monotonic();forward=_Forward();pending=b''
        while True:
            if time.monotonic()-last_tick>1.0: raise RuntimeError('controller heartbeat timed out')
            readable,_,_=select.select([sys.stdin],[],[],.01)
            if readable:
                chunk=os.read(sys.stdin.fileno(),4096)
                if not chunk:return
                pending+=chunk
                if len(pending)>4096: raise ValueError('worker command overflow')
                while b'\n' in pending:
                    command,pending=pending.split(b'\n',1)
                    if command==b'stop':return
                    if command!=b'tick':raise ValueError('invalid worker command')
                    last_tick=time.monotonic()
            host.pump(forward)
    finally:host.restore_local()

if __name__=='__main__':
    try:main()
    except BaseException as exc:
        try:_send(dict(type='error',message=str(exc)))
        except Exception:pass
        sys.exit(1)
