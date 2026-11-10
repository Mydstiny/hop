"""Explicit, read-only monitor query in a fresh process."""
import json
import sys

def main():
    from .x11 import X11Backend,initialize_xlib_threads
    initialize_xlib_threads()
    args=sys.argv[1:]
    if args not in ([],['--require-input-clear'],['--require-input-clear','--pointer']):raise ValueError('invalid probe mode')
    with X11Backend() as backend:
        if args and not backend.input_state_clear():raise RuntimeError('keys, buttons or Caps Lock active')
        screens=backend.screens()
        result={'screens':screens,'pointer':backend.pointer_position()} if '--pointer' in args else screens
        print(json.dumps(result,separators=(',',':')))

if __name__=='__main__':
    try:main()
    except BaseException:sys.exit(1)
