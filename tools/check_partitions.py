"""Validate the custom layout against Arduino 3.3.11's fixed upload regions."""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
FLASH_BYTES = 16 * 1024 * 1024
BOOT_APP_START = 0xE000
BOOT_APP_END = 0x10000


@dataclass(frozen=True)
class Partition:
    name: str
    kind: int
    subtype: int
    offset: int
    size: int
    flags: int = 0


def number(value: str) -> int:
    value = value.strip()
    if value.lower().endswith(('k', 'm')):
        multiplier = 1024 if value[-1].lower() == 'k' else 1024 * 1024
        return int(value[:-1], 0) * multiplier
    return int(value, 0)


def read_csv(path: Path) -> list[Partition]:
    result = []
    lines = (line for line in path.read_text(encoding='utf-8-sig').splitlines()
             if line.strip() and not line.lstrip().startswith('#'))
    for row in csv.reader(lines):
        if len(row) < 5 or len(row) > 6:
            raise ValueError('Partition rows must contain 5 or 6 fields.')
        fields = [value.strip() for value in row]
        name, kind, subtype, offset, size = fields[:5]
        kinds = {'app': 0, 'data': 1}
        subtypes = {(0, 'factory'): 0, (1, 'nvs'): 2}
        kind_value = kinds[kind] if kind in kinds else number(kind)
        subtype_value = subtypes.get((kind_value, subtype))
        if subtype_value is None:
            subtype_value = number(subtype)
        flags = 0 if len(fields) == 5 or not fields[5] else number(fields[5])
        result.append(Partition(name, kind_value, subtype_value,
                                number(offset), number(size), flags))
    return result


def validate(partitions: list[Partition]) -> None:
    if len(partitions) != 2:
        raise ValueError('Only NVS and the factory app are allowed; no exercise-data partitions.')
    if len({entry.name for entry in partitions}) != len(partitions):
        raise ValueError('Duplicate partition names.')
    for entry in partitions:
        end = entry.offset + entry.size
        if entry.offset < 0x9000 or entry.size <= 0 or end > FLASH_BYTES:
            raise ValueError(f'{entry.name}: outside usable flash or invalid size.')
        if entry.offset % 0x1000 or entry.size % 0x1000:
            raise ValueError(f'{entry.name}: partition must align to flash sectors.')
        if entry.offset < BOOT_APP_END and end > BOOT_APP_START:
            raise ValueError(f'{entry.name}: overlaps Arduino boot_app0 upload at 0xe000..0xffff.')
        if entry.flags:
            raise ValueError(f'{entry.name}: unexpected partition flags.')
    ordered = sorted(partitions, key=lambda item: item.offset)
    for left, right in zip(ordered, ordered[1:]):
        if left.offset + left.size > right.offset:
            raise ValueError(f'{left.name} overlaps {right.name}.')
    expected = [Partition('nvs', 1, 2, 0x9000, 0x5000),
                Partition('factory', 0, 0, 0x10000, 0x400000)]
    if ordered != expected:
        raise ValueError('Expected NVS 0x9000+0x5000 and factory app 0x10000+0x400000.')


def read_binary(path: Path) -> list[Partition]:
    data = path.read_bytes()
    result = []
    for offset in range(0, len(data), 32):
        entry = data[offset:offset + 32]
        if len(entry) != 32:
            raise ValueError('Truncated binary partition table.')
        if entry == b'\xff' * 32:
            return result
        if entry[:2] == b'\xeb\xeb':
            if entry[2:16] != b'\xff' * 14 or entry[16:] != hashlib.md5(data[:offset]).digest():
                raise ValueError('Invalid partition table MD5.')
            if any(byte != 0xFF for byte in data[offset + 32:]):
                raise ValueError('Unexpected data after partition table MD5.')
            return result
        magic, kind, subtype, address, size, name, flags = struct.unpack('<HBBII16sI', entry)
        if magic != 0x50AA:
            raise ValueError('Invalid binary partition entry magic.')
        result.append(Partition(name.split(b'\0', 1)[0].decode('ascii'),
                                kind, subtype, address, size, flags))
    raise ValueError('Binary partition table has no terminator.')


def check_sdkconfig(path: Path) -> None:
    lines = path.read_text(encoding='utf-8').splitlines()
    if 'CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=y' in lines:
        raise ValueError('This layout requires PHY initialization data embedded in firmware.')
    if '# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set' not in lines:
        raise ValueError('Cannot verify that PHY initialization data is embedded.')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--csv', type=Path, default=ROOT / 'firmware/RehabEsp32/partitions.csv')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--sdkconfig', type=Path)
    parser.add_argument('--boot-app0', type=Path)
    parser.add_argument('--app', type=Path)
    args = parser.parse_args()
    try:
        partitions = read_csv(args.csv)
        validate(partitions)
        if args.binary and sorted(read_binary(args.binary), key=lambda item: item.offset) != sorted(partitions, key=lambda item: item.offset):
            raise ValueError('Compiled binary partition table does not match partitions.csv.')
        if args.sdkconfig:
            check_sdkconfig(args.sdkconfig)
        if args.boot_app0 and args.boot_app0.stat().st_size != BOOT_APP_END - BOOT_APP_START:
            raise ValueError('Unexpected boot_app0.bin size; recheck Arduino upload reservations.')
        if args.app and args.app.stat().st_size > next(item.size for item in partitions if item.kind == 0):
            raise ValueError('Application image exceeds its factory partition.')
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, f'Partition check failed: {error}\n')
    print('Partition check passed: NVS ends at 0xe000; Arduino upload region is reserved; factory app is 4 MiB.')


if __name__ == '__main__':
    main()
