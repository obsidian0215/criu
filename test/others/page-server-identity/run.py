#!/usr/bin/env python3
"""Exercise remote-parent identity through CRIU's sender and page-server."""

import argparse
import contextlib
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import time
import uuid

import pycriu.images

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CRIU = Path(os.environ.get("CRIU", str(ROOT / "criu/criu"))).resolve()
WORK = HERE / "work"
PE_PARENT = 1
PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")


def run(*args):
    subprocess.run([str(CRIU), "--no-default-config", *map(str, args)], check=True, timeout=60)


def image(path):
    with path.open("rb") as stream:
        return pycriu.images.load(stream)


def save_image(path, data):
    temp = path.with_suffix(".tmp")
    with temp.open("wb") as stream:
        pycriu.images.dump(data, stream)
    temp.replace(path)


def change_generation(directory, generation):
    for path in directory.glob("pagemap-*.img"):
        data = image(path)
        if generation is None:
            data["entries"][0].pop("memory_generation_id", None)
        else:
            data["entries"][0]["memory_generation_id"] = generation
        save_image(path, data)


def inherited_pages(directory, pid):
    return sum(entry["nr_pages"] for entry in image(directory / f"pagemap-{pid}.img")["entries"][1:]
               if entry.get("in_parent", False) or entry.get("flags", 0) & PE_PARENT)


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError("page-server closed the connection")
        data += part
    return data


@contextlib.contextmanager
def page_server(directory, parent=None):
    directory.mkdir()
    client, server = socket.socketpair()
    command = [str(CRIU), "--no-default-config", "page-server", "-D", str(directory),
               "-o", "page-server.log", "-v4", "--ps-socket", str(server.fileno())]
    if parent is not None:
        command += ["--prev-images-dir", os.path.relpath(parent, directory)]
    process = subprocess.Popen(command, pass_fds=(server.fileno(),))
    server.close()
    try:
        yield client, process
    finally:
        client.close()
        if process.poll() is None:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


class Case:
    def __init__(self, name):
        self.directory = WORK / name
        self.directory.mkdir(parents=True)
        self.pid = None
        self.process = None
        self.name = name
        self.address = 0
        self.expected = b""

    def start(self):
        state = self.directory / "state"
        output = (self.directory / "workload.log").open("wb")
        self.process = subprocess.Popen([str(HERE / "workload"), str(state)],
                                        stdin=subprocess.DEVNULL, stdout=output,
                                        stderr=output, start_new_session=True)
        output.close()
        for _ in range(200):
            if state.exists() and state.read_text().strip():
                break
            if self.process.poll() is not None:
                raise AssertionError("workload exited before initialization")
            time.sleep(0.01)
        pid, address, size = state.read_text().split()
        self.pid, self.address = int(pid), int(address, 16)
        self.expected = bytes((i // PAGE_SIZE * 17 + i % 251) & 255 for i in range(int(size)))
        self.verify()

    def verify(self):
        with open(f"/proc/{self.pid}/mem", "rb", buffering=0) as memory:
            assert os.pread(memory.fileno(), len(self.expected), self.address) == self.expected, self.name

    def transfer(self, label, mode="splice", source_parent=None, target_parent=None, final=False):
        source = self.directory / ("source-" + label)
        target = self.directory / ("target-" + label)
        source.mkdir()
        with page_server(target, target_parent) as (client, server):
            command = [str(CRIU), "--no-default-config", "dump" if final else "pre-dump",
                       "-D", str(source), "-o", "dump.log", "-v4", "-t", str(self.pid),
                       "--track-mem", "--page-server", "--ps-socket", str(client.fileno())]
            if not final:
                command += ["--pre-dump-mode", mode]
            if source_parent is not None:
                command += ["--prev-images-dir", os.path.relpath(source_parent, source)]
            subprocess.run(command, pass_fds=(client.fileno(),), check=True, timeout=60)
            client.close()
            assert server.wait(timeout=20) == 0
        assert not list(source.glob("pages-*.img")), "source kept memory payload"
        assert not list(source.glob("pagemap-*.img")), "source kept coverage"
        if final:
            self.process.wait(timeout=10)
            self.pid = None
            for path in source.iterdir():
                if path.is_file() and path.suffix == ".img" and not path.name.startswith(("pages-", "pagemap-")):
                    shutil.copy2(path, target / path.name)
        return source, target

    def restore(self, target):
        pidfile = self.directory / "restored.pid"
        run("restore", "-D", target, "-o", "restore.log", "-v4", "-d", "--pidfile", pidfile)
        self.pid = int(pidfile.read_text())
        self.verify()

    def close(self):
        if self.pid is not None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.pid = None
        if self.process is not None:
            self.process.wait(timeout=10)


def predump_parent(mode, kind):
    case = Case(f"predump-{mode}-{kind}")
    try:
        case.start()
        source, target = case.transfer("one", mode)
        if kind == "mismatch":
            change_generation(target, str(uuid.uuid4()))
        elif kind == "legacy":
            change_generation(target, None)
        source, target = case.transfer("two", mode, source, target)
        count = inherited_pages(target, case.pid)
        assert (count > 0) if kind == "match" else (count == 0), (case.name, count)
        _, target = case.transfer("final", mode, source, target, final=True)
        case.restore(target)
        print("PASS", case.name, flush=True)
    finally:
        case.close()


def broken_ancestor():
    case = Case("broken-ancestor")
    try:
        case.start()
        source_a, target_a = case.transfer("one")
        source_b, target_b = case.transfer("two", source_parent=source_a, target_parent=target_a)
        assert inherited_pages(target_b, case.pid) > 0
        change_generation(target_a, str(uuid.uuid4()))
        old_pid = case.pid
        _, target_c = case.transfer("final", source_parent=source_b, target_parent=target_b, final=True)
        assert inherited_pages(target_c, old_pid) == 0
        assert (target_c / "parent").is_symlink()
        case.restore(target_c)
        print("PASS", case.name, flush=True)
    finally:
        case.close()


def changed_parent():
    case = Case("changed-after-query")
    try:
        case.start()
        source, target = case.transfer("one")
        parent = image(source / "inventory.img")["entries"][0]["memory_generation_id"]
        current = str(uuid.uuid4())
        generation = struct.pack("=I37s37s2x", 3, current.encode(), parent.encode())
        dst_id = case.pid << 8 | 1
        with page_server(case.directory / "target-race", target) as (sock, server):
            sock.settimeout(20)
            sock.sendall(struct.pack("=I4xQQQ", 10, 0, 0, dst_id) + generation)
            assert struct.unpack("=i", recv_exact(sock, 4))[0] == 1
            change_generation(target, str(uuid.uuid4()))
            sock.sendall(struct.pack("=I4xQQQ", 9, 0, 0, dst_id) + generation)
            assert recv_exact(sock, 1) == b"\0"
            sock.sendall(struct.pack("=I4xQQQ", 6 | PE_PARENT << 16, 1, case.address, dst_id))
            sock.shutdown(socket.SHUT_WR)
            assert server.wait(timeout=20) != 0, "server accepted inherited pages without a parent"
        print("PASS", case.name, flush=True)
    finally:
        case.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=["predump", "ancestor", "race", "all"], default="all")
    args = parser.parse_args()
    feature = subprocess.run([str(CRIU), "--no-default-config", "check",
                              "--feature", "mem_dirty_track"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=30)
    if feature.returncode:
        print("SKIP: page-server identity requires memory dirty tracking")
        print(feature.stdout)
        return
    if WORK.exists():
        raise SystemExit(f"Remove previous test output before running: {WORK}")
    WORK.mkdir()
    if args.case in ("predump", "all"):
        for mode in ("splice", "read"):
            for kind in ("match", "mismatch", "legacy"):
                predump_parent(mode, kind)
    if args.case in ("ancestor", "all"):
        broken_ancestor()
    if args.case in ("race", "all"):
        changed_parent()


if __name__ == "__main__":
    try:
        main()
    except Exception:
        for log in sorted(WORK.rglob("*.log")):
            print(f"--- {log} ---", flush=True)
            print("\n".join(log.read_text(errors="replace").splitlines()[-40:]), flush=True)
        raise
