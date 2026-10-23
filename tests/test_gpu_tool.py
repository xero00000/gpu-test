"""Host-only regression checks: no module loading or boot changes."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
from unittest import mock

SPEC = importlib.util.spec_from_file_location('gpu_tool', Path(__file__).resolve().parents[1] / 'scripts/gpu_tool.py')
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class ToolTests(unittest.TestCase):
    def test_device_selection_requires_full_explicit_address(self):
        for bad in ('all', '*', '05:00.0', '0000:05:20.0', '../../dev/mem'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                TOOL.card(bad)

    def test_turing_requires_exact_board_and_bus(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            def fixture(bdf, device='0x1e81', sub='0x3081'):
                p = root / bdf
                p.mkdir(exist_ok=True)
                for k, v in dict(vendor='0x10de', device=device, subsystem_vendor='0x3842', subsystem_device=sub).items():
                    (p / k).write_text(v)
            fixture('0000:05:00.0')
            self.assertEqual(TOOL.card('0000:05:00.0', root)['family'], 'tu104')
            fixture('0000:06:00.0')
            with self.assertRaises(ValueError):
                TOOL.card('0000:06:00.0', root)
            fixture('0000:05:00.0', sub='0x3082')
            with self.assertRaises(ValueError):
                TOOL.card('0000:05:00.0', root)
            fixture('0000:05:00.0', device='0x1e87')
            with self.assertRaises(ValueError):
                TOOL.card('0000:05:00.0', root)

    def test_ampere_requires_a_qualified_board(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            p = root / '0000:2b:00.0'
            p.mkdir()
            for k, v in dict(vendor='0x10de', device='0x2216', subsystem_vendor='0x19da', subsystem_device='0x1612').items():
                (p / k).write_text(v)
            self.assertEqual(TOOL.card('0000:2b:00.0', root)['family'], 'ampere')
            (p / 'subsystem_device').write_text('0x1613')
            with self.assertRaises(ValueError):
                TOOL.card('0000:2b:00.0', root)

    def test_trial_preserves_kernel_and_root_options(self):
        text = 'title Normal\nversion 1\nlinux /vmlinuz-1\ninitrd /normal.img\noptions root=UUID=test ro quiet\n'
        targets = [dict(bdf='0000:2b:00.0', family='ampere'), dict(bdf='0000:05:00.0', family='tu104')]
        result = TOOL.boot_entry(text, Path('/boot/trial.img'), targets)
        self.assertIn('linux /vmlinuz-1\n', result)
        self.assertIn('options root=UUID=test ro quiet gpu_unlock.targets=0000:2b:00.0 gpu_unlock_tu104.targets=0000:05:00.0\n', result)
        self.assertIn('initrd /trial.img\n', result)

    def test_bounded_command_times_out(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            TOOL.run([sys.executable, '-c', 'import time; time.sleep(5)'], timeout=.1)

    def test_ambiguous_or_preexisting_unlock_entry_refused(self):
        text = 'title Normal\nlinux /vmlinuz-1\ninitrd /normal.img\noptions root=test ro\n'
        bad = [text + 'initrd /another.img\n', text.replace('options root=test ro', 'options $kernelopts'),
               text.replace('options root=test ro', 'options root=test gpu_unlock.targets=all'),
               text.replace('initrd /normal.img', 'initrd /microcode.img /normal.img')]
        for entry in bad:
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                TOOL.boot_entry(entry, Path('/boot/test.img'), [])

    def test_kit_rejects_changed_or_escaping_assets(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            kit = root / 'kit'
            kit.mkdir()
            (root / 'outside').write_bytes(b'original')
            asset = kit / 'asset'
            asset.write_bytes(b'original')
            data = dict(schema=1, status='built_and_host_tests_passed', plan={}, modules=[], firmware=[],
                        diagnostics=dict(file='asset', sha256=TOOL.digest(asset)))
            (kit / 'kit.json').write_text(json.dumps(data))
            with mock.patch.object(TOOL, 'validate_plan'):
                TOOL.validate_kit(kit)
                asset.write_bytes(b'tampered')
                with self.assertRaises(ValueError):
                    TOOL.validate_kit(kit)
                data['diagnostics']['file'] = '../outside'
                (kit / 'kit.json').write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    TOOL.validate_kit(kit)


if __name__ == '__main__':
    unittest.main()
