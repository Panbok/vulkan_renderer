#!/usr/bin/env python3
"""Exercise the `vkr_bakery serve` daemon protocol on small texture sources.

Covers runs with tagged events, watches with one background rebuild per
edit (the daemon's own outputs do not retrigger it), cancellation of a
running command, disconnection, a second daemon for the same socket, stale
socket recovery, shutdown and idle exit.
"""
import argparse
import json
import os
from pathlib import Path
import random
import socket
import struct
import subprocess
import tempfile
import time
import zlib

import project_jobs as jobs


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
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
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
    process = subprocess.Popen([str(bakery), 'serve', '--root', str(root), '--socket',
                                str(socket_path), *extra],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               env={**os.environ, 'VKR_BAKERY_CACHE': str(cache)})
    listening = json.loads(process.stdout.readline())
    assert listening['ev'] == 'listening' and listening['socket'] == str(socket_path), listening
    return process


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
        socket_path = Path('/tmp') / f'vkr-serve-check-{os.getpid()}.sock'
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
            assert done and done[0]['outputs'] == [str(sources / 'small.png.vkt')]
            assert (sources / 'small.png.vkt').is_file()
            rejected = client.send('run', argv=['project', '--request', 'x'])
            assert client.reply(rejected)[-1]['exit'] == 2

            # One edit reports one change and one rebuild; the rebuild's own
            # output does not report another change.
            watch_request = client.send('watch', paths=['src'], argv=['cook', 'src'])
            watch = client.reply(watch_request)[-1]['watch']
            time.sleep(0.5)
            write_png(sources / 'small.png', 16, 16, 2)
            events = client.until(lambda e: e.get('ev') == 'reply' and e.get('watch') == watch)
            changed = [event for event in events if event.get('ev') == 'changed']
            assert len(changed) == 1 and changed[0]['paths'] == [str(sources / 'small.png')], changed
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
            stopped.terminate()
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
