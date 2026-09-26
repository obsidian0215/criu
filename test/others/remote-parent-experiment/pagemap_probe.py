#!/usr/bin/env python3
import argparse
from pathlib import Path
import sys

PE_PARENT = 1
PE_PRESENT = 4


def load_entries(directory: str, image_id: int):
    import pycriu.images
    path = Path(directory) / f'pagemap-{image_id}.img'
    with path.open('rb') as image:
        return pycriu.images.load(image)['entries'][1:]


def classify(entries, address: int, page_size: int) -> str:
    found = []
    for entry in entries:
        start = int(entry['vaddr'])
        end = start + int(entry.get('nr_pages', 0)) * page_size
        if start <= address < end:
            found.append(entry)
    if not found:
        return 'absent'
    if len(found) != 1:
        raise RuntimeError(f'{address:#x} is covered by {len(found)} entries')
    flags = int(found[0].get('flags', 0))
    if flags & PE_PRESENT:
        return 'present'
    if flags & PE_PARENT:
        return 'parent'
    return f'other:{flags}'


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('directory')
    parser.add_argument('image_id', type=int)
    parser.add_argument('address', type=lambda value: int(value, 0))
    parser.add_argument('expected', nargs='?')
    args = parser.parse_args()
    value = classify(load_entries(args.directory, args.image_id), args.address,
                     __import__('os').sysconf('SC_PAGE_SIZE'))
    print(value)
    if args.expected and value != args.expected:
        raise RuntimeError(f'{args.address:#x}: {value}, expected {args.expected}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
