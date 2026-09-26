#!/usr/bin/env python3
import argparse
import json
import socket
import threading
import time
from pathlib import Path

RESPONSE_SIZE = 4


def shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def relay_source(source, target, stop, stats):
    total = 0
    try:
        while not stop.is_set():
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
            total += len(data)
    except OSError:
        pass
    finally:
        stats['source_to_server'] = total
        stop.set()
        shutdown(target)


def relay_responses(target, source, stop, stats, delay_seconds):
    total = 0
    messages = 0
    pending = bytearray()
    try:
        while not stop.is_set():
            data = target.recv(65536)
            if not data:
                break
            pending.extend(data)
            while len(pending) >= RESPONSE_SIZE:
                response = bytes(pending[:RESPONSE_SIZE])
                del pending[:RESPONSE_SIZE]
                if delay_seconds:
                    time.sleep(delay_seconds)
                source.sendall(response)
                total += RESPONSE_SIZE
                messages += 1
    except OSError:
        pass
    finally:
        stats['server_to_source'] = total
        stats['response_messages'] = messages
        stats['partial_response_bytes'] = len(pending)
        stop.set()
        shutdown(source)
        shutdown(target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--listen-port', type=int, required=True)
    parser.add_argument('--target-port', type=int, required=True)
    parser.add_argument('--response-delay-ms', type=float, default=0.0)
    parser.add_argument('--stats', type=Path, required=True)
    parser.add_argument('--ready', type=Path, required=True)
    args = parser.parse_args()
    if args.response_delay_ms < 0:
        parser.error('--response-delay-ms must not be negative')

    stats = {
        'source_to_server': 0,
        'server_to_source': 0,
        'response_messages': 0,
        'partial_response_bytes': 0,
        'response_delay_ms': args.response_delay_ms,
    }
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', args.listen_port))
        listener.listen(1)
        args.ready.write_text('ready\n')
        source, _ = listener.accept()

    with source, socket.create_connection(('127.0.0.1', args.target_port)) as target:
        stop = threading.Event()
        threads = [
            threading.Thread(target=relay_source,
                             args=(source, target, stop, stats)),
            threading.Thread(target=relay_responses,
                             args=(target, source, stop, stats,
                                   args.response_delay_ms / 1000.0)),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

    temporary = args.stats.with_suffix(args.stats.suffix + '.tmp')
    temporary.write_text(json.dumps(stats, sort_keys=True) + '\n')
    temporary.replace(args.stats)


if __name__ == '__main__':
    main()
