"""Guard the settings region against Arduino's unconditional boot_app0 upload."""
from dataclasses import replace
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_partitions import (Partition, check_sdkconfig, read_binary, read_csv, validate)


class PartitionTests(unittest.TestCase):
    def setUp(self):
        self.layout = read_csv(ROOT / 'firmware/RehabEsp32/partitions.csv')

    def test_repository_layout_reserves_upload_area(self):
        validate(self.layout)
        self.assertEqual(self.layout[0].offset + self.layout[0].size, 0xE000)
        self.assertEqual(self.layout[1].offset, 0x10000)

    def test_original_nvs_overlap_is_rejected(self):
        layout = [replace(self.layout[0], size=0x6000), self.layout[1]]
        with self.assertRaisesRegex(ValueError, 'boot_app0'):
            validate(layout)

    def test_old_phy_partition_is_rejected(self):
        with self.assertRaises(ValueError):
            validate(self.layout + [Partition('phy_init', 1, 1, 0xF000, 0x1000)])

    def test_factory_cannot_overlap_uploader(self):
        layout = [self.layout[0], replace(self.layout[1], offset=0xF000)]
        with self.assertRaisesRegex(ValueError, 'boot_app0'):
            validate(layout)

    def test_no_filesystem_or_coredump_partition(self):
        for name, subtype in [('storage', 0x82), ('coredump', 3)]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate(self.layout + [Partition(name, 1, subtype, 0x410000, 0x10000)])

    def test_bad_layout_boundaries(self):
        for changes in [dict(size=0), dict(offset=0x8000), dict(offset=0x9100),
                        dict(size=0x5100), dict(flags=1), dict(size=16 * 1024 * 1024)]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate([replace(self.layout[0], **changes), self.layout[1]])
        with self.assertRaises(ValueError):
            validate([self.layout[0], replace(self.layout[1], size=0x800000)])

    def test_binary_table_and_checksum(self):
        entries = b''.join(struct.pack('<HBBII16sI', 0x50AA, item.kind, item.subtype,
                                      item.offset, item.size, item.name.encode(), item.flags)
                           for item in self.layout)
        checksum = b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(entries).digest()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'partitions.bin'
            path.write_bytes(entries + checksum + b'\xff' * 64)
            self.assertEqual(read_binary(path), self.layout)
            corrupt = bytearray(entries + checksum)
            corrupt[8] ^= 1
            path.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError, 'MD5'):
                read_binary(path)
            path.write_bytes(entries[:-1])
            with self.assertRaisesRegex(ValueError, 'Truncated'):
                read_binary(path)

    def test_phy_must_be_embedded(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'sdkconfig'
            path.write_text('# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set\n', encoding='utf-8')
            check_sdkconfig(path)
            for text in ['CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=y\n', '']:
                path.write_text(text, encoding='utf-8')
                with self.assertRaises(ValueError):
                    check_sdkconfig(path)


if __name__ == '__main__':
    unittest.main()
