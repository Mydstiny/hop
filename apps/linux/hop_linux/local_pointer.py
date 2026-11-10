"""Internal explicit pointer placement after returning local focus."""
import sys

def main():
    if len(sys.argv)!=3:raise ValueError('position required')
    x,y=(int(v) for v in sys.argv[1:])
    from .x11 import X11Backend,initialize_xlib_threads
    initialize_xlib_threads()
    with X11Backend() as backend:backend.motion(x,y)

if __name__=='__main__':
    try:main()
    except BaseException:sys.exit(1)
