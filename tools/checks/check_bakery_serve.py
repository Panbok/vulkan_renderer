#!/usr/bin/env python3
"""Exercise the `vkr_bakery serve` daemon protocol on small texture sources.

Covers runs with tagged events, watches with one background rebuild per
edit (the daemon's own outputs do not retrigger it), cancellation of a
running command, disconnection, a second daemon for the same socket, stale
socket recovery, shutdown and idle exit.

Runs on macOS and Windows. Windows Python has no socket.AF_UNIX, so there the
client calls Winsock's AF_UNIX through ctypes.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import signal
import socket
import struct
import subprocess
import tempfile
import time
import zlib

import project_jobs as jobs

# Seconds a watch event may take to arrive; see the watch case in main().
WATCH_DELIVERY_TIMEOUT = 120.0
WINDOWS = os.name == 'nt'


def same_path(a, b):
    """The daemon reports '/' paths; Windows also ignores case."""
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


class WinsockUnixSocket:
    """The part of socket.socket the checks use, over Winsock AF_UNIX."""

    AF_UNIX = 1
    SOL_SOCKET = 0xffff
    SO_RCVTIMEO = 0x1006
    WSAETIMEDOUT = 10060

    class Address(ctypes.Structure):
        _fields_ = [('family', ctypes.c_ushort), ('path', ctypes.c_char * 108)]

    def __init__(self):
        ws2 = ctypes.WinDLL('ws2_32')
        ws2.socket.restype = ctypes.c_size_t
        ws2.connect.argtypes = [ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
        ws2.send.argtypes = [ctypes.c_size_t, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        ws2.recv.argtypes = [ctypes.c_size_t, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        ws2.setsockopt.argtypes = [ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_void_p, ctypes.c_int]
        ws2.closesocket.argtypes = [ctypes.c_size_t]
        # WSADATA is about 400 bytes; Winsock stays started for the process.
        if ws2.WSAStartup(0x0202, ctypes.create_string_buffer(1024)) != 0:
            raise OSError('WSAStartup failed')
        self.ws2 = ws2
        self.handle = ws2.socket(self.AF_UNIX, socket.SOCK_STREAM, 0)
        if self.handle == ctypes.c_size_t(-1).value:
            raise OSError(ws2.WSAGetLastError(), 'socket(AF_UNIX) failed')

    def connect(self, path):
        address = self.Address(self.AF_UNIX, str(path).encode())
        if self.ws2.connect(self.handle, ctypes.byref(address), ctypes.sizeof(address)) != 0:
            raise ConnectionError(self.ws2.WSAGetLastError(), f'cannot connect to {path}')

    def settimeout(self, seconds):
        milliseconds = ctypes.c_uint32(max(1, int(seconds * 1000)))
        self.ws2.setsockopt(self.handle, self.SOL_SOCKET, self.SO_RCVTIMEO,
                            ctypes.byref(milliseconds), ctypes.sizeof(milliseconds))

    def sendall(self, data):
        while data:
            sent = self.ws2.send(self.handle, data, len(data), 0)
            if sent < 0:
                raise ConnectionError(self.ws2.WSAGetLastError(), 'send failed')
            data = data[sent:]

    def recv(self, size):
        buffer = ctypes.create_string_buffer(size)
        received = self.ws2.recv(self.handle, buffer, size, 0)
        if received < 0:
            error = self.ws2.WSAGetLastError()
            if error == self.WSAETIMEDOUT:
                raise socket.timeout('timed out')
            raise ConnectionError(error, 'recv failed')
        return buffer.raw[:received]

    def close(self):
        self.ws2.closesocket(self.handle)


def write_png(path, width, height, seed):
    """Writes an RGB PNG; seeded noise keeps encodes from being trivial."""
    generator = random.Random(seed)
    row = bytes(generator.getrandbits(8) for _ in range(width * 3))
    raw = b''.join(b'\0' + row[y % 7:] + row[:y % 7] for y in range(height))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))


class Client:
    def __init__(self, path):
        if hasattr(socket, 'AF_UNIX'):
            self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        else:
            self.socket = WinsockUnixSocket()
        self.socket.connect(str(path))
        self.buffer = b''
        self.next_id = 1

    def send(self, request, **fields):
        identifier = self.next_id
        self.next_id += 1
        line = json.dumps({'v': 1, 'id': identifier, 'req': request, **fields}) + '\n'
        self.socket.sendall(line.encode())
        return identifier

    def read(self, timeout=30.0):
        deadline = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError('Timed out waiting for the daemon')
            self.socket.settimeout(remaining)
            data = self.socket.recv(65536)
            if not data:
                raise AssertionError('The daemon closed the connection')
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line)

    def until(self, predicate, timeout=30.0):
        events = []
        while True:
            event = self.read(timeout)
            events.append(event)
            if predicate(event):
                return events

    def reply(self, identifier, timeout=30.0):
        return self.until(lambda e: e.get('ev') == 'reply' and e.get('req') == identifier,
                          timeout)

    def close(self):
        self.socket.close()


def start(bakery, root, socket_path, cache, *extra):
    # Its own process group on Windows, so stop() can raise SIGBREAK in it.
    process = subprocess.Popen([str(bakery), 'serve', '--root', str(root), '--socket',
                                str(socket_path), *extra],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               encoding='utf-8',
                               env={**os.environ, 'VKR_BAKERY_CACHE': str(cache)},
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if WINDOWS else 0)
    listening = json.loads(process.stdout.readline())
    assert listening['ev'] == 'listening' and listening['socket'] == str(socket_path), listening
    return process


def stop(process):
    """SIGTERM; Windows has none, and the daemon takes Ctrl+Break as it."""
    if WINDOWS:
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        process.terminate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-serve-') as temporary:
        root = Path(temporary).resolve()
        cache = root / 'cache'
        sources = root / 'src'
        sources.mkdir()
        write_png(sources / 'small.png', 16, 16, 1)
        # A short socket path: Unix socket names are limited to ~100 bytes.
        socket_directory = Path(tempfile.gettempdir()) if WINDOWS else Path('/tmp')
        socket_path = socket_directory / f'vkr-serve-check-{os.getpid()}.sock'
        daemon = start(bakery, root, socket_path, cache, '--idle-exit', '60')
        try:
            client = Client(socket_path)
            ping = client.send('ping')
            assert client.reply(ping)[-1]['exit'] == 0

            # A run streams tagged events and ends with its exit code.
            run = client.send('run', argv=['cook', 'src/small.png'])
            events = client.reply(run)
            assert events[-1]['exit'] == 0, events
            assert all(event.get('req') == run for event in events)
            done = [event for event in events if event.get('ev') == 'done']
            assert done and len(done[0]['outputs']) == 1, done
            assert same_path(done[0]['outputs'][0], sources / 'small.png.vkt'), done
            assert (sources / 'small.png.vkt').is_file()
            rejected = client.send('run', argv=['project', '--request', 'x'])
            assert client.reply(rejected)[-1]['exit'] == 2

            # One edit reports one change and one rebuild; the rebuild's own
            # output does not report another change.
            watch_request = client.send('watch', paths=['src'], argv=['cook', 'src'])
            # The reply means the watcher is armed: an edit right after it is seen.
            watch = client.reply(watch_request)[-1]['watch']
            write_png(sources / 'small.png', 16, 16, 2)
            # FSEvents delivery queues behind fseventsd, which a parallel build's
            # file churn can keep busy: changes arrived 2-14 s late at load 21.
            events = client.until(lambda e: e.get('ev') == 'reply' and e.get('watch') == watch,
                                  timeout=WATCH_DELIVERY_TIMEOUT)
            changed = [event for event in events if event.get('ev') == 'changed']
            assert (len(changed) == 1 and len(changed[0]['paths']) == 1 and
                    same_path(changed[0]['paths'][0], sources / 'small.png')), changed
            assert events[-1]['exit'] == 0
            client.socket.settimeout(2.0)
            try:
                extra = client.read(timeout=2.0)
                raise AssertionError(f'Unexpected event after the rebuild: {extra}')
            except (AssertionError, socket.timeout) as error:
                if 'Unexpected' in str(error):
                    raise
            unwatch = client.send('unwatch', watch=watch)
            assert client.reply(unwatch)[-1]['exit'] == 0

            # Cancelling a running encode ends it with the cancelled status.
            write_png(sources / 'large.png', 2048, 2048, 3)
            slow = client.send('run', argv=['cook', 'src/large.png'])
            client.until(lambda e: e.get('ev') == 'start' and e.get('req') == slow)
            cancel = client.send('cancel', target=slow)
            replies = {}
            while slow not in replies or cancel not in replies:
                event = client.read(60)
                if event.get('ev') == 'reply':
                    replies[event['req']] = event['exit']
            assert replies[cancel] == 0 and replies[slow] == 3, replies
            assert not (sources / 'large.png.vkt').exists()
            assert not list((cache / 'tmp').iterdir())

            # A second daemon for the same socket refuses to start.
            second = subprocess.run([str(bakery), 'serve', '--root', str(root), '--socket',
                                     str(socket_path)], capture_output=True, text=True, timeout=30)
            assert second.returncode != 0 and 'already serves' in second.stderr

            # A disconnected client's watches end with it.
            other = Client(socket_path)
            other_watch = other.reply(other.send('watch', paths=['src']))[-1]['watch']
            other.close()
            time.sleep(0.5)
            again = client.send('unwatch', watch=other_watch)
            assert client.reply(again)[-1]['exit'] == 1

            # The command-line client prints events until its reply.
            printed = subprocess.run([str(bakery), 'send', '--socket', str(socket_path),
                                      json.dumps({'v': 1, 'id': 7, 'req': 'run',
                                                  'argv': ['status', 'src/small.png']})],
                                     capture_output=True, text=True, timeout=60)
            lines = [json.loads(line) for line in printed.stdout.splitlines()]
            assert printed.returncode == 0 and lines[-1] == {'v': 1, 'ev': 'reply', 'req': 7, 'exit': 0}

            shutdown = client.send('shutdown')
            assert client.reply(shutdown)[-1]['exit'] == 0
            assert daemon.wait(timeout=30) == 0
            assert not socket_path.exists()
        finally:
            if daemon.poll() is None:
                daemon.terminate()
                daemon.wait()

        # A stale socket file is replaced; an idle daemon exits by itself.
        socket_path.write_text('stale')
        idle = start(bakery, root, socket_path, cache, '--idle-exit', '1')
        assert idle.wait(timeout=30) == 0
        assert not socket_path.exists()

        # SIGTERM during a command cancels it and stops the daemon, not only
        # the command.
        stopped = start(bakery, root, socket_path, cache)
        try:
            client = Client(socket_path)
            slow = client.send('run', argv=['cook', 'src/large.png'])
            client.until(lambda e: e.get('ev') == 'start' and e.get('req') == slow)
            stop(stopped)
            assert client.reply(slow, timeout=60)[-1]['exit'] == 3
            assert stopped.wait(timeout=30) == 0
            assert not socket_path.exists()
            assert not (sources / 'large.png.vkt').exists()
            assert not list((cache / 'tmp').iterdir())
        finally:
            if stopped.poll() is None:
                stopped.kill()
                stopped.wait()
    print('Bakery daemon: tagged runs, watches without self-triggering, cancellation, '
          'disconnection, single daemon per socket, stale socket, shutdown, idle exit '
          'and SIGTERM passed')


if __name__ == '__main__':
    main()
