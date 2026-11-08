#!/usr/bin/env python3
"""Actual Swift server ↔ actual HarmonyOS C++ Client+Injector, recording fakes on localhost only."""
from pathlib import Path
import queue
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'artifacts/test-build'
SERVER = ROOT / 'apps/macos/build/tests/headless-server'


class Process:
    def __init__(self, args):
        self.process = subprocess.Popen([str(a) for a in args], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, bufsize=1)
        self.lines = queue.Queue()
        self.history = []
        def read():
            for line in self.process.stdout:
                self.lines.put(line.strip())
            self.lines.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def line(self, timeout=6):
        line = self.lines.get(timeout=timeout)
        self.history.append(line)
        if line is None:
            raise AssertionError(f'Unexpected subprocess EOF: {self.history}')
        return line

    def until(self, expected, timeout=6):
        deadline = time.monotonic() + timeout
        while True:
            line = self.line(max(.01, deadline - time.monotonic()))
            if line == expected:
                return
            if line.startswith(('ERROR', 'FAIL')) or time.monotonic() >= deadline:
                raise AssertionError(f'Expected {expected}, got {self.history}')

    def command(self, text):
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=6)
        self.reader.join(timeout=2)
        self.process.stdin.close()
        self.process.stdout.close()


def scenario(executable, direction):
    server, client = None, None
    try:
        server = Process([SERVER, '--direction', direction])
        first = server.line()
        assert first.startswith('LISTEN '), first
        client = Process([executable, first.split()[1]])
        client.until('CHALLENGE')
        # Actual C++ client auto-confirms only its TEST panel, but the actual Swift
        # state machine must remain unbound until explicitly confirmed here.
        server.command('status')
        assert server.line() == 'STATE unauthenticated unbound'
        try:
            premature = client.line(timeout=.15)
        except queue.Empty:
            premature = None
        assert premature is None, f'Native token/persist before mutual consent: {premature}'
        server.command('confirm')
        assert server.line() == 'CONFIRMED'
        client.until('PERSISTED_IN_MEMORY')
        client.until('READY')
        server.command('status')
        assert server.line() == 'STATE authenticated bound'
        server.command('enter')
        assert server.line() == 'ENTER_SENT'
        server.until('ENTERED')
        client.until('REMOTE_HELD')
        server.command('exercise')
        assert server.line() == 'EXERCISED'
        server.command('return')
        assert server.line() == 'RETURN_SENT'
        server.until('RETURNED')
        # The actual Client MUST release before REQUEST_LEAVE/its state event and
        # then accept actual Swift LEAVE; memory-only assertions inspect fake SDK records.
        client.until('LEAVING_RELEASED')
        client.until('LOCAL_RELEASED')
        client.command('quit')
        client.until('PASS_RECORDED_INPUT_AND_RELEASE')
        assert client.process.wait(timeout=6) == 0
        server.command('quit')
        assert server.process.wait(timeout=6) == 0
        layout = 'left' if direction == 0 else 'right'
        print(f'PASS native interop ({layout}): mutual pairing + commit ACK + ENTER exact ACK + held-state injection')
        print(f'PASS native interop ({layout}): drag/buttons/2-axis wheel/repeat/modifiers/Caps + return-edge release before LOCAL')
    finally:
        if client:
            client.cleanup()
        if server:
            server.cleanup()


def main():
    if not SERVER.is_file():
        raise SystemExit('Run bash apps/macos/scripts/test.sh to build the safe headless Swift test server first.')
    OUT.mkdir(parents=True, exist_ok=True)
    native = ROOT / 'apps/harmonyos/entry/src/main/cpp'
    executable = OUT / 'interop-client'
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                    '-I', str(ROOT / 'apps/harmonyos/tests/fake'), '-I', str(native),
                    str(ROOT / 'tools/interop_client.cpp'), str(native / 'client.cpp'),
                    '-o', str(executable)], check=True)
    for direction in (0, 1):
        scenario(executable, direction)
    print('PASS: 2 actual Swift↔C++ input/return scenarios; SDK injection calls use recording fakes ONLY.')
    print('No AppKit app/event tap/physical input/Keychain/Preferences/device injection was used.')


if __name__ == '__main__':
    main()
