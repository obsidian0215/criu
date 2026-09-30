#!/usr/bin/env python3
"""Paired all-remote CRIU benchmark; each run must restore and verify every byte."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import signal
import socket
import subprocess
import time
import traceback

import pycriu.images

HERE = Path(__file__).resolve().parent
PAGE_SIZE = os.sysconf('SC_PAGE_SIZE')


def load_image(path):
    with path.open('rb') as stream:
        return pycriu.images.load(stream)


def command(binary, *args):
    return [str(binary), '--no-default-config', *map(str, args)]


def directory_size(directory):
    files = [p for p in directory.glob('*.img') if p.is_file()]
    return {'apparent_bytes': sum(p.stat().st_size for p in files),
            'allocated_bytes': sum(p.stat().st_blocks * 512 for p in files),
            'page_payload_bytes': sum(p.stat().st_size for p in files if p.name.startswith('pages-'))}


def inherited_pages(directory, pid):
    return sum(entry['nr_pages'] for entry in
               load_image(directory / f'pagemap-{pid}.img')['entries'][1:]
               if entry.get('in_parent', False) or entry.get('flags', 0) & 1)


def tcp_pair():
    # Direct TCP loopback, without a copying relay or traffic shaping.
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    client = socket.create_connection(listener.getsockname())
    server, _ = listener.accept()
    listener.close()
    return client, server


def wait_state(path, epoch):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            fields = path.read_text().split()
            if len(fields) == 4 and int(fields[3]) == epoch:
                return int(fields[0]), int(fields[1], 16), int(fields[2])
        except (FileNotFoundError, ValueError):
            pass
        time.sleep(0.005)
    raise RuntimeError(f'Workload did not acknowledge epoch {epoch}')


def verify_memory(pid, address, size, dirty, epoch):
    actual = hashlib.sha256()
    expected = hashlib.sha256()
    changed_pages = (size // PAGE_SIZE) * dirty // 100
    with open(f'/proc/{pid}/mem', 'rb', buffering=0) as stream:
        for offset in range(0, size, 1024 * 1024):
            chunk = os.pread(stream.fileno(), min(1024 * 1024, size - offset), address + offset)
            if len(chunk) != min(1024 * 1024, size - offset):
                raise AssertionError('Short external memory read')
            actual.update(chunk)
    for page in range(size // PAGE_SIZE):
        value = (page * 17 + 3 + (epoch if page < changed_pages else 0)) & 255
        expected.update(bytes([value]) * PAGE_SIZE)
    if actual.digest() != expected.digest():
        raise AssertionError(f'Restored byte mismatch: {actual.hexdigest()} != {expected.hexdigest()}')
    return actual.hexdigest()


def transfer(binary, directory, pid, index, mode, source_parent, target_parent, final):
    source = directory / f'source-{index}'
    target = directory / f'target-{index}'
    source.mkdir()
    target.mkdir()
    client, server_socket = tcp_pair()
    server_command = command(binary, 'page-server', '-D', target, '-W', target, '-o', 'page-server.log',
                             '-v1', '--ps-socket', server_socket.fileno())
    if target_parent:
        server_command += ['--prev-images-dir', os.path.relpath(target_parent, target)]
    server = subprocess.Popen(server_command, pass_fds=(server_socket.fileno(),))
    dump_command = command(binary, 'dump' if final else 'pre-dump', '-D', source, '-W', source,
                           '-o', 'dump.log', '-v1', '-t', pid, '--track-mem',
                           '--page-server', '--ps-socket', client.fileno())
    if not final:
        dump_command += ['--pre-dump-mode', mode]
    if source_parent:
        dump_command += ['--prev-images-dir', os.path.relpath(source_parent, source)]
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic_ns()
    try:
        subprocess.run(dump_command, pass_fds=(client.fileno(),), check=True, timeout=120)
        dump_return = time.monotonic_ns()
        client.shutdown(socket.SHUT_WR)
        if server.wait(timeout=30):
            raise AssertionError('Page server failed')
        destination_ready = time.monotonic_ns()
        usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        tcp_metrics = json.loads(subprocess.check_output(
            [str(HERE / 'tcp-metrics'), str(client.fileno()), str(server_socket.fileno())],
            pass_fds=(client.fileno(), server_socket.fileno()), text=True))
    finally:
        client.close()
        server_socket.close()
        if server.poll() is None:
            server.kill()
            server.wait()
    if list(source.glob('pages-*.img')) or list(source.glob('pagemap-*.img')):
        raise AssertionError('Source retained memory images')
    stats_path = source / 'stats-dump'
    stats = load_image(stats_path)
    dump_stats = stats['entries'][0]['dump']
    result = {'index': index, 'final': final,
              'sender_elapsed_ms': (dump_return - start) / 1e6,
              'destination_ready_ms': (destination_ready - start) / 1e6,
              'child_user_cpu_s': usage_after.ru_utime - usage_before.ru_utime,
              'child_system_cpu_s': usage_after.ru_stime - usage_before.ru_stime,
              'child_output_blocks': usage_after.ru_oublock - usage_before.ru_oublock,
              'source': directory_size(source), 'target': directory_size(target),
              'inherited_pages': inherited_pages(target, pid), 'stats': stats,
              'tcp': tcp_metrics}
    print(json.dumps({'round': index, 'final': final, 'payload_bytes': result['target']['page_payload_bytes'],
                      'inherited_pages': result['inherited_pages'], 'dump_stats': dump_stats,
                      'tcp': tcp_metrics, 'destination_ready_ms': result['destination_ready_ms']}), flush=True)
    return source, target, result, start


def run_case(args, binary, route, dirty, repetition, warmup=False):
    directory = args.output / f'{"warmup-" if warmup else ""}{args.mode}-{dirty}-{repetition}-{route}'
    directory.mkdir()
    result = {'route': route, 'dirty_percent': dirty, 'repetition': repetition,
              'mode': args.mode, 'memory_mib': args.memory_mib, 'warmup': warmup,
              'status': 'running', 'rounds': []}
    state = directory / 'state'
    workload = None
    pid = None
    try:
        with (directory / 'workload.log').open('wb') as output:
            workload = subprocess.Popen([str(HERE / 'workload'), str(state),
                                         str(args.memory_mib), str(dirty)],
                                        stdin=subprocess.DEVNULL, stdout=output,
                                        stderr=output, start_new_session=True)
        pid, address, size = wait_state(state, 0)
        verify_memory(pid, address, size, dirty, 0)
        source_parent = target_parent = None
        for index in range(args.predumps + 1):
            if index:
                os.kill(pid, signal.SIGUSR1)
                wait_state(state, index)
            final = index == args.predumps
            # This control deliberately sends a full final generation, using the
            # same candidate binary and otherwise identical pre-dump history.
            no_parent = final and route == 'candidate-full-final'
            source, target, metrics, start = transfer(
                binary, directory, pid, index, args.mode,
                None if no_parent else source_parent,
                None if no_parent else target_parent, final)
            result['rounds'].append(metrics)
            if index and not no_parent and metrics['inherited_pages'] == 0:
                raise AssertionError('Valid-parent round did not inherit any pages')
            if no_parent and metrics['inherited_pages']:
                raise AssertionError('Full-final control inherited pages')
            source_parent, target_parent = source, target
        workload.wait(timeout=10)
        pid = None
        for path in source.iterdir():
            if path.is_file() and path.suffix == '.img' and not path.name.startswith(('pages-', 'pagemap-')):
                shutil.copy2(path, target / path.name)
        result['final_metadata_ready_ms'] = (time.monotonic_ns() - start) / 1e6
        pidfile = directory / 'restored.pid'
        restore_start = time.monotonic_ns()
        subprocess.run(command(binary, 'restore', '-D', target, '-W', target, '-o', 'restore.log',
                               '-v1', '-d', '--pidfile', pidfile), check=True, timeout=120)
        result['restore_ms'] = (time.monotonic_ns() - restore_start) / 1e6
        pid = int(pidfile.read_text())
        result['restored_sha256'] = verify_memory(pid, address, size, dirty, args.predumps)
        result['final_start_to_external_verification_ms'] = (time.monotonic_ns() - start) / 1e6
        result['status'] = 'passed'
        return result
    except Exception:
        result['status'] = 'failed'
        result['error'] = traceback.format_exc()
        raise
    finally:
        if pid:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if workload and workload.poll() is None:
            workload.kill()
            workload.wait()
        (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        with (args.output / 'results.jsonl').open('a') as stream:
            stream.write(json.dumps(result) + '\n')
        # Retain logs and decoded counters, but discard successful payloads to
        # keep the full matrix within runner/artifact storage limits.
        if result['status'] == 'passed':
            for path in directory.rglob('*.img'):
                path.unlink()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--memory-mib', type=int, default=256)
    parser.add_argument('--predumps', type=int, default=3)
    parser.add_argument('--repetitions', type=int, default=5)
    parser.add_argument('--dirty', nargs='+', type=int, default=[0, 1, 20])
    parser.add_argument('--mode', choices=['splice', 'read'], default='splice')
    parser.add_argument('--full-final-control', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.baseline = args.baseline.resolve()
    args.candidate = args.candidate.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    environment = {'platform': platform.platform(), 'page_size': PAGE_SIZE,
                   'arguments': {key: str(value) if isinstance(value, Path) else value
                                 for key, value in vars(args).items()},
                   'transport': 'direct TCP loopback; no traffic shaping',
                   'wire_bytes': 'TCP_INFO data octets in both directions; includes CRIU protocol control, excludes TCP/IP headers; retransmissions separately recorded'}
    for name, path in [('cpuinfo', '/proc/cpuinfo'), ('meminfo', '/proc/meminfo'),
                       ('mountinfo', '/proc/self/mountinfo'), ('cpu_max', '/sys/fs/cgroup/cpu.max')]:
        try:
            environment[name] = Path(path).read_text()
        except OSError:
            pass
    (args.output / 'environment.json').write_text(json.dumps(environment, indent=2) + '\n')
    for binary in (args.baseline, args.candidate):
        subprocess.run(command(binary, 'check', '--feature', 'mem_dirty_track'), check=True)
    routes = [('baseline', args.baseline), ('candidate', args.candidate)]
    if args.full_final_control:
        routes.append(('candidate-full-final', args.candidate))
    for route, binary in routes:
        run_case(args, binary, route, args.dirty[0], -1, warmup=True)
    for dirty in args.dirty:
        for repetition in range(args.repetitions):
            ordered = routes if repetition % 2 == 0 else list(reversed(routes))
            for route, binary in ordered:
                print(f'RUN {args.mode} dirty={dirty}% pair={repetition} {route}', flush=True)
                run_case(args, binary, route, dirty, repetition)
    print('PASS: every measured run restored and passed full external memory verification', flush=True)


if __name__ == '__main__':
    main()
