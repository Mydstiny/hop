"""Private X11 edge sampler subprocess; stdin closure terminates the worker."""
import json
import os
import select
import sys
import time
from dataclasses import asdict
from .local_edge import EdgeSampler


def main():
    from .x11 import X11Backend,initialize_xlib_threads
    initialize_xlib_threads()
    os.set_blocking(sys.stdout.fileno(),False)
    def emit(item):
        data=(json.dumps(item,separators=(',',':'))+'\n').encode()
        if os.write(sys.stdout.fileno(),data)!=len(data):raise RuntimeError('edge pipe full')
    with X11Backend() as backend:
        screens=backend.screens();sampler=EdgeSampler(screens);last_check=time.monotonic()
        emit({'type':'ready','screens':screens})
        while True:
            ready,_,_=select.select([sys.stdin.fileno()],[],[],1/120)
            if ready:
                data=os.read(sys.stdin.fileno(),128)
                if not data:return
                if data!=b'stop\n':raise RuntimeError('invalid edge worker command')
                return
            now=time.monotonic()
            if now-last_check>=.5:
                if backend.screens()!=screens:raise RuntimeError('display configuration changed')
                last_check=now
            event=sampler.sample(*backend.pointer_position())
            if event:emit({'type':'edge',**asdict(event)})


if __name__=='__main__':
    try:main()
    except BaseException:sys.exit(1)
