#!/usr/bin/env python3
import argparse
import json
import socket
import threading
from pathlib import Path


def relay(source, destination, counter, key):
    total = 0
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            destination.sendall(data)
            total += len(data)
    finally:
        counter[key] = total
        try:
            destination.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--listen-port', type=int, required=True)
    parser.add_argument('--target-port', type=int, required=True)
    parser.add_argument('--stats', type=Path, required=True)
    parser.add_argument('--ready', type=Path, required=True)
    args = parser.parse_args()

    counts = {'source_to_server': 0, 'server_to_source': 0}
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', args.listen_port))
        listener.listen(1)
        args.ready.write_text('ready\n')
        source, _ = listener.accept()

    with source, socket.create_connection(('127.0.0.1', args.target_port)) as target:
        threads = [
            threading.Thread(target=relay, args=(source, target, counts, 'source_to_server')),
            threading.Thread(target=relay, args=(target, source, counts, 'server_to_source')),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

    temporary = args.stats.with_suffix(args.stats.suffix + '.tmp')
    temporary.write_text(json.dumps(counts, sort_keys=True) + '\n')
    temporary.replace(args.stats)


if __name__ == '__main__':
    main()
