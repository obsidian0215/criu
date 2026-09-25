#!/usr/bin/env python3
import argparse
import glob
import json
import os
from pathlib import Path
import struct
import sys

TOP = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(TOP / 'lib'))

PE_PARENT = 1
PE_PRESENT = 4


def normalize_entry(entry):
    flags = int(entry.get('flags', 0))
    if entry.get('in_parent'):
        flags |= PE_PARENT
    elif 'flags' not in entry:
        flags |= PE_PRESENT
    nr_pages = int(entry.get('nr_pages', entry.get('compat_nr_pages', 0)))
    return int(entry['vaddr']), nr_pages, flags


def load_entries(directory, pid):
    import pycriu.images
    path = Path(directory) / f'pagemap-{pid}.img'
    with path.open('rb') as stream:
        image = pycriu.images.load(stream)
    return [normalize_entry(entry) for entry in image['entries'][1:]]


def image_stats(directory, pid, address, size):
    page_size = os.sysconf('SC_PAGE_SIZE')
    if address % page_size or size % page_size:
        raise ValueError('range is not page aligned')
    start = address
    end = address + size
    present = parent = other = 0
    intervals = []
    for base, nr_pages, flags in load_entries(directory, pid):
        entry_end = base + nr_pages * page_size
        overlap_start = max(start, base)
        overlap_end = min(end, entry_end)
        if overlap_start >= overlap_end:
            continue
        pages = (overlap_end - overlap_start) // page_size
        intervals.append((overlap_start, overlap_end))
        if flags & PE_PRESENT:
            present += pages
        elif flags & PE_PARENT:
            parent += pages
        else:
            other += pages
    intervals.sort()
    covered = 0
    cursor = start
    for left, right in intervals:
        left = max(left, cursor)
        if right <= left:
            continue
        if left > cursor:
            cursor = left
        covered += (right - left) // page_size
        cursor = max(cursor, right)
    total = size // page_size
    return {
        'total_pages': total,
        'present_pages': present,
        'parent_pages': parent,
        'other_pages': other,
        'covered_pages': covered,
        'uncovered_pages': total - covered,
    }


def proc_stats(pid, address, size):
    page_size = os.sysconf('SC_PAGE_SIZE')
    if address % page_size or size % page_size:
        raise ValueError('range is not page aligned')
    present = swapped = softdirty = 0
    with open(f'/proc/{pid}/pagemap', 'rb', buffering=0) as stream:
        for offset in range(0, size, page_size):
            index = (address + offset) // page_size
            value = os.pread(stream.fileno(), 8, index * 8)
            if len(value) != 8:
                raise RuntimeError(f'short pagemap read for page {index}')
            entry = struct.unpack('Q', value)[0]
            present += bool(entry & (1 << 63))
            swapped += bool(entry & (1 << 62))
            softdirty += bool(entry & (1 << 55))
    return {
        'total_pages': size // page_size,
        'present_pages': present,
        'swapped_pages': swapped,
        'softdirty_pages': softdirty,
    }


def shmem_payload(directory):
    import pycriu.images
    directory = Path(directory)
    rows = []
    total = 0
    for name in sorted(glob.glob(str(directory / 'pagemap-shmem-*.img'))):
        path = Path(name)
        with path.open('rb') as stream:
            image = pycriu.images.load(stream)
        head = image['entries'][0]
        pages_id = int(head['pages_id'])
        pages_path = directory / f'pages-{pages_id}.img'
        size = pages_path.stat().st_size if pages_path.exists() else 0
        total += size
        rows.append({'pagemap': path.name, 'pages_id': pages_id,
                     'pages_file': pages_path.name, 'payload_bytes': size})
    return {'payload_bytes': total, 'images': rows}


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    proc = sub.add_parser('proc-stats')
    proc.add_argument('pid', type=int)
    proc.add_argument('address', type=int)
    proc.add_argument('size', type=int)
    image = sub.add_parser('image-stats')
    image.add_argument('directory')
    image.add_argument('pid', type=int)
    image.add_argument('address', type=int)
    image.add_argument('size', type=int)
    shmem = sub.add_parser('shmem-payload')
    shmem.add_argument('directory')
    args = parser.parse_args()
    if args.command == 'proc-stats':
        result = proc_stats(args.pid, args.address, args.size)
    elif args.command == 'image-stats':
        result = image_stats(args.directory, args.pid, args.address, args.size)
    else:
        result = shmem_payload(args.directory)
    json.dump(result, sys.stdout, sort_keys=True)
    sys.stdout.write('\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
