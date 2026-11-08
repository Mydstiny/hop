#!/usr/bin/env python3
"""Actual Swift server + actual HarmonyOS C++ Client pairing on localhost, no physical input."""
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
            if line in ('ERROR', 'FAIL') or time.monotonic() >= deadline:
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
    server, client = None, None
    try:
        server = Process([SERVER])
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
        client.command('quit')
        client.until('PASS_NO_INPUT')
        assert client.process.wait(timeout=6) == 0
        server.command('quit')
        assert server.process.wait(timeout=6) == 0
        print('PASS native interop: actual Swift Network/TCP/pairing + actual HarmonyOS C++ Client')
        print('PASS native interop: one-sided confirmation cannot persist; mutual consent + commit ACK + READY agree')
        print('No AppKit app/event tap/physical input/Keychain/Preferences/device injection was used.')
    finally:
        if client:
            client.cleanup()
        if server:
            server.cleanup()


if __name__ == '__main__':
    main()
