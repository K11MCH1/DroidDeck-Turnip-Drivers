import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import package
from pipeline import apply_series


class PipelineTests(unittest.TestCase):
    def test_all_manifest_paths_are_vendored(self):
        config = json.loads((ROOT / 'patches.json').read_text())
        paths = config['extra'] + config['linux']
        for series in config['banner'].values():
            paths += series
        for path in paths:
            self.assertTrue((ROOT / path).is_file(), path)

    def test_patch_failures_stop_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            import subprocess
            with patch('pipeline.subprocess.run', side_effect=subprocess.CalledProcessError(1, ['patch'])):
                with self.assertRaises(subprocess.CalledProcessError):
                    apply_series(Path(directory), ['vendor/banners/patches/8g2_oneui.py'])

    def test_patch_path_cannot_escape_recipe(self):
        with self.assertRaises(ValueError):
            apply_series(ROOT, ['../meson.build'])

    def test_elf_rejects_android_and_wrong_architecture(self):
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<HH', header, 16, 3, 183)
        with self.assertRaisesRegex(ValueError, 'glibc'):
            package.verify_elf(header, '(NEEDED) Shared library: [libc.so]', ROOT)
        struct.pack_into('<H', header, 18, 62)
        with self.assertRaisesRegex(ValueError, 'AArch64'):
            package.verify_elf(header, '', ROOT)

    def test_glibc_requirement_uses_numeric_version(self):
        self.assertEqual('2.39', package.min_glibc('f@GLIBC_2.9 g@GLIBC_2.39'))
        with self.assertRaises(ValueError):
            package.min_glibc('f@GLIBC_2.40')
        with self.assertRaises(ValueError):
            package.min_glibc('')


if __name__ == '__main__':
    unittest.main()
