"""Read-only monitor and input-state query in a separate process.

The GTK process must not open Xlib connections of its own (the Xlib error
handler is process-global and XInitThreads must precede any Xlib use), so
queries run here. Two modes:

- one-shot: ``python -m hop_linux.monitor_probe [--require-input-clear [--pointer]]``
  prints one JSON value and exits;
- ``--serve``: keeps one X11 connection open and answers one JSON request per
  stdin line, so an edge crossing does not wait for an interpreter to start.
  Closing stdin ends it. The only write is ``warp``, which moves this
  computer's own pointer when focus returns to it.
"""
import json
import os
import sys

_MAX_REQUEST = 4096


def _snapshot(backend, clear, pointer):
    if clear and not backend.input_state_clear():
        return {'ok': False, 'busy': True}
    reply = {'ok': True, 'screens': [list(s) for s in backend.screens()]}
    if pointer:
        reply['pointer'] = list(backend.pointer_position())
    return reply


def _serve(backend):
    out = sys.stdout.fileno()

    def emit(item):
        data = (json.dumps(item, separators=(',', ':')) + '\n').encode()
        if os.write(out, data) != len(data):
            raise RuntimeError('probe pipe full')

    emit({'type': 'ready'})
    stream = sys.stdin.buffer
    while True:
        raw = stream.readline(_MAX_REQUEST + 1)
        if not raw:
            return
        if len(raw) > _MAX_REQUEST or not raw.endswith(b'\n'):
            raise RuntimeError('invalid probe request')
        request = json.loads(raw)
        op = request.get('op') if isinstance(request, dict) else None
        if op == 'snapshot':
            clear, pointer = request.get('clear'), request.get('pointer')
            if type(clear) is not bool or type(pointer) is not bool or (pointer and not clear):
                raise RuntimeError('invalid snapshot request')
            emit(_snapshot(backend, clear, pointer))
        elif op == 'warp':
            x, y = request.get('x'), request.get('y')
            if type(x) is not int or type(y) is not int or not (0 <= x <= 32767 and 0 <= y <= 32767):
                raise RuntimeError('invalid warp request')
            backend.motion(x, y)
            emit({'ok': True})
        else:
            raise RuntimeError('unknown probe request')


def main():
    from .x11 import X11Backend, initialize_xlib_threads
    initialize_xlib_threads()
    args = sys.argv[1:]
    if args == ['--serve']:
        with X11Backend() as backend:
            _serve(backend)
        return
    if args not in ([], ['--require-input-clear'], ['--require-input-clear', '--pointer']):
        raise ValueError('invalid probe mode')
    with X11Backend() as backend:
        reply = _snapshot(backend, bool(args), '--pointer' in args)
        if not reply['ok']:
            raise RuntimeError('keys or buttons held')
        print(json.dumps(reply['screens'] if not args or '--pointer' not in args else
                         {'screens': reply['screens'], 'pointer': reply['pointer']}, separators=(',', ':')))


if __name__ == '__main__':
    try:
        main()
    except BaseException:
        sys.exit(1)
