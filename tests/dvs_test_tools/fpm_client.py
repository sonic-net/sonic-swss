#!/usr/bin/env python3
"""Persistent synthetic FRR peer, run INSIDE the DVS container by its fixture.

One socket is retained across commands: reconnecting per message would flush
fpmsyncd-owned mappings and defeat dependency-order tests. Control/ack are atomic
JSON files; connection or send errors are surfaced to the test, never retried by
silently establishing a new FPM session.
"""
import argparse
import base64
import json
from pathlib import Path
import socket
import time


def atomic(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value))
    temporary.replace(path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('directory', type=Path)
    p.add_argument('--port', type=int, default=2620)
    args = p.parse_args()
    directory = args.directory
    directory.mkdir(parents=True, exist_ok=True)
    # Wait only for initial listener readiness. Once established, an error
    # never creates a replacement session or silently flushes learned rows.
    deadline = time.monotonic()+10
    while True:
        try:
            connection = socket.create_connection(('127.0.0.1', args.port), timeout=1)
            break
        except OSError:
            if time.monotonic() >= deadline: raise
            time.sleep(0.1)
    with connection:
        connection.settimeout(10)
        atomic(directory/'ack.json', {'sequence': 0, 'connected': True})
        sequence = 0
        while True:
            command = directory/'command.json'
            if not command.exists():
                time.sleep(0.01)
                continue
            data = json.loads(command.read_text())
            if data['sequence'] <= sequence:
                time.sleep(0.01)
                continue
            sequence = data['sequence']
            try:
                if data.get('close'):
                    atomic(directory/'ack.json', {'sequence': sequence, 'closed': True})
                    return
                connection.sendall(base64.b64decode(data['frame'], validate=True))
                atomic(directory/'ack.json', {'sequence': sequence, 'sent': True})
            except (OSError, ValueError) as error:
                atomic(directory/'ack.json', {'sequence': sequence, 'error': str(error)})
                raise


if __name__ == '__main__':
    main()
