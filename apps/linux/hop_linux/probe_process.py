"""A long-lived ``monitor_probe --serve`` child for the controller.

An edge crossing used to start up to three Python interpreters (input check,
second input check, pointer warp) while the session loop was stalled. One child
keeps an X11 connection open instead; each query is one line each way.
"""
import json
import os
import select
import subprocess
import sys
import threading
import time

_MAX_REPLY = 1 << 16


class ProbeError(RuntimeError):
    pass


class ProbeBusy(ProbeError):
    """A key or mouse button is held on this computer."""


class X11Probe:
    def __init__(self, command=None, timeout=2.0):
        self.command = list(command or [sys.executable, '-B', '-m', 'hop_linux.monitor_probe', '--serve'])
        self.timeout = timeout
        self.lock = threading.Lock()
        self.process = None
        self.buffer = b''

    def snapshot(self, clear=False, pointer=False):
        if pointer and not clear:
            raise ValueError('pointer probe requires input-state check')
        reply = self._request({'op': 'snapshot', 'clear': clear, 'pointer': pointer})
        if reply.get('busy') is True and reply.get('ok') is False:
            raise ProbeBusy('keys or buttons held')
        if reply.get('ok') is not True:
            raise ProbeError('invalid probe reply')
        return reply

    def warp(self, x, y):
        if type(x) is not int or type(y) is not int:
            raise ValueError('integer pointer position required')
        if self._request({'op': 'warp', 'x': x, 'y': y}).get('ok') is not True:
            raise ProbeError('pointer warp failed')

    def _request(self, item):
        line = (json.dumps(item, separators=(',', ':')) + '\n').encode()
        with self.lock:
            # A child that died since the last query (X server reset, display
            # change) is replaced once; a second failure is reported.
            for attempt in (0, 1):
                try:
                    self._ensure()
                    self.process.stdin.write(line)
                    self.process.stdin.flush()
                    return self._read_line()
                except (OSError, ValueError, ProbeError):
                    self._stop()
                    if attempt:
                        raise ProbeError('local X11 probe failed') from None

    def _ensure(self):
        if self.process is not None and self.process.poll() is None:
            return
        self._stop()
        self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, bufsize=0)
        if self._read_line().get('type') != 'ready':
            raise ProbeError('probe did not start')

    def _read_line(self):
        deadline = time.monotonic() + self.timeout
        fd = self.process.stdout.fileno()
        while b'\n' not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProbeError('probe timed out')
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                raise ProbeError('probe exited')
            self.buffer += chunk
            if len(self.buffer) > _MAX_REPLY:
                raise ProbeError('probe reply too long')
        raw, self.buffer = self.buffer.split(b'\n', 1)
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ProbeError('invalid probe reply')
        return value

    def _stop(self):
        process, self.process, self.buffer = self.process, None, b''
        if process is None:
            return
        try:
            process.stdin.close()
        except OSError:
            pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
        process.stdout.close()

    def close(self):
        with self.lock:
            self._stop()
