#!/usr/bin/env python3
import argparse
import json
import socket
import threading
from pathlib import Path


def shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def relay_source(source, target, stop, counts):
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
        counts['source_to_server'] = total
        stop.set()
        shutdown(target)


def relay_server(target, source, stop, counts, limit):
    total = 0
    try:
        while not stop.is_set():
            data = target.recv(65536)
            if not data:
                break
            if limit >= 0:
                remaining = limit - total
                if remaining <= 0:
                    break
                data = data[:remaining]
            source.sendall(data)
            total += len(data)
            if limit >= 0 and total >= limit:
                break
    except OSError:
        pass
    finally:
        counts['server_to_source'] = total
        stop.set()
        shutdown(source)
        shutdown(target)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--listen-port', type=int, required=True)
    parser.add_argument('--target-port', type=int, required=True)
    parser.add_argument('--server-bytes', type=int, required=True)
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
        stop = threading.Event()
        threads = [
            threading.Thread(target=relay_source,
                             args=(source, target, stop, counts)),
            threading.Thread(target=relay_server,
                             args=(target, source, stop, counts,
                                   args.server_bytes)),
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
