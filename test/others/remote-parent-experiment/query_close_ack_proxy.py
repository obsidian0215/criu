#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import threading
from pathlib import Path

FRAME_SIZE = 32
PARENT_RANGE_SIZE = 16
PS_CMD_MASK = (1 << 16) - 1
PS_IOV_PARENT_RANGES = 9
PS_IOV_CLOSE = 0x1023
PS_IOV_FORCE_CLOSE = 0x1024


def shutdown(sock):
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def relay_source(source, target, close_seen, stop, counts):
    total = 0
    pending = bytearray()
    payload_remaining = 0
    try:
        while not stop.is_set():
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
            total += len(data)
            pending.extend(data)

            while pending:
                if payload_remaining:
                    consumed = min(payload_remaining, len(pending))
                    del pending[:consumed]
                    payload_remaining -= consumed
                    if payload_remaining:
                        break
                    continue

                if len(pending) < FRAME_SIZE:
                    break

                frame = bytes(pending[:FRAME_SIZE])
                del pending[:FRAME_SIZE]
                command = struct.unpack_from('=I', frame)[0] & PS_CMD_MASK

                if command == PS_IOV_PARENT_RANGES:
                    nr_ranges = struct.unpack_from('=Q', frame, 8)[0]
                    payload_remaining = nr_ranges * PARENT_RANGE_SIZE
                elif command in (PS_IOV_CLOSE, PS_IOV_FORCE_CLOSE):
                    close_seen.set()
    except OSError:
        pass
    finally:
        counts['source_to_server'] = total
        stop.set()
        shutdown(target)


def relay_server(target, source, close_seen, stop, counts):
    total = 0
    try:
        while not stop.is_set():
            data = target.recv(65536)
            if not data:
                break
            if close_seen.is_set():
                counts['close_response_dropped'] = True
                break
            source.sendall(data)
            total += len(data)
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
    parser.add_argument('--stats', type=Path, required=True)
    parser.add_argument('--ready', type=Path, required=True)
    args = parser.parse_args()

    counts = {
        'source_to_server': 0,
        'server_to_source': 0,
        'close_response_dropped': False,
    }
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', args.listen_port))
        listener.listen(1)
        args.ready.write_text('ready\n')
        source, _ = listener.accept()

    with source, socket.create_connection(('127.0.0.1', args.target_port)) as target:
        close_seen = threading.Event()
        stop = threading.Event()
        threads = [
            threading.Thread(target=relay_source,
                             args=(source, target, close_seen, stop, counts)),
            threading.Thread(target=relay_server,
                             args=(target, source, close_seen, stop, counts)),
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
