"""Host-only P2P regression checks; never load a driver or change boot settings."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('gpu_p2p', ROOT / 'scripts/gpu_p2p.py')
P2P = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(P2P)

def sha(data): return hashlib.sha256(data).hexdigest()

class P2PTests(unittest.TestCase):
    def binary_fixture(self):
        pins = copy.deepcopy(P2P.PINS)
        data = bytearray(max(x['offset'] + len(bytes.fromhex(x['before'])) for x in pins['libcuda']['changes']))
        for site in pins['libcuda']['changes']:
            old = bytes.fromhex(site['before'])
            data[site['offset']:site['offset']+len(old)] = old
        patched = data.copy()
        for site in pins['libcuda']['changes']:
            new = bytes.fromhex(site['after'])
            patched[site['offset']:site['offset']+len(new)] = new
        pins['libcuda']['source_sha256'] = sha(data)
        pins['libcuda']['candidate_sha256'] = sha(patched)
        return data, patched, pins

    def test_five_branches_preserve_targets_and_only_eight_bytes_change(self):
        data, expected, pins = self.binary_fixture()
        with mock.patch.object(P2P, 'PINS', pins):
            actual = P2P.patch_bytes(data)
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), len(data))
        self.assertEqual(sum(a != b for a,b in zip(data,actual)), 8)

    def test_unknown_binary_and_bad_branch_or_target_are_refused(self):
        data, _, pins = self.binary_fixture()
        with self.assertRaises(ValueError): P2P.patch_bytes(b'unknown binary')
        bad = copy.deepcopy(pins)
        bad['libcuda']['changes'][0]['before'] = '7445'
        with mock.patch.object(P2P, 'PINS', bad), self.assertRaises(ValueError): P2P.patch_bytes(data)
        bad = copy.deepcopy(pins)
        bad['libcuda']['changes'][-1]['target'] += 1
        with mock.patch.object(P2P, 'PINS', bad), self.assertRaises(ValueError): P2P.patch_bytes(data)

    def test_private_library_keeps_input_and_refuses_existing_output(self):
        data, expected, pins = self.binary_fixture()
        with tempfile.TemporaryDirectory() as d, mock.patch.object(P2P, 'PINS', pins):
            root=Path(d);source=root/'original.so';source.write_bytes(data);out=root/'private'
            result=P2P.patch_library(source,out)
            self.assertEqual(source.read_bytes(),data)
            self.assertEqual(Path(result['candidate']).read_bytes(),expected)
            self.assertEqual((out/'libcuda.so.1').resolve(),out/'libcuda.so.595.104.02')
            with self.assertRaises(ValueError): P2P.patch_library(source,out)

    def test_system_outputs_are_refused(self):
        for directory in ['/usr/lib64/test-p2p', '/boot/test-p2p', '/etc/test-p2p', '/var/lib/test-p2p', '/opt/test-p2p']:
            with self.subTest(directory=directory), self.assertRaises(ValueError): P2P.private_output(Path(directory))

    def source_fixture(self, root):
        source=root/'input';source.mkdir()
        (source/'version.mk').write_text('NVIDIA_VERSION = 595.104.02\n')
        (source/'target.c').write_text('old\n')
        (source/'distro-fix.c').write_text('keep my previous patch\n')
        package=root/'package';package.mkdir()
        patch=package/'ampere-p2p-595.104.02.patch'
        patch.write_text('--- a/target.c\n+++ b/target.c\n@@ -1 +1 @@\n-old\n+new\n--- /dev/null\n+++ b/new.h\n@@ -0,0 +1 @@\n+header\n')
        pins=copy.deepcopy(P2P.PINS)
        pins.update(patch_sha256=P2P.digest(patch),source_files={'target.c':dict(before=sha(b'old\n'),after=sha(b'new\n')),'new.h':dict(before=None,after=sha(b'header\n'))})
        return source,package,pins

    @unittest.skipUnless(shutil.which('patch'), 'patch is required')
    def test_source_preparation_preserves_unrelated_patches_and_input(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);source,package,pins=self.source_fixture(root)
            before=P2P.snapshot(source)
            with mock.patch.object(P2P,'PINS',pins),mock.patch.object(P2P,'PACKAGE',package):
                result=P2P.prepare_source(source,root/'out')
            self.assertEqual(P2P.snapshot(source),before)
            self.assertTrue(result['original_unchanged'])
            self.assertEqual((root/'out/source/distro-fix.c').read_text(),'keep my previous patch\n')
            self.assertEqual((root/'out/source/target.c').read_text(),'new\n')

    def test_conflicting_patch_and_escaping_symlink_are_refused(self):
        for kind in ['overlap','symlink']:
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                root=Path(d);source,package,pins=self.source_fixture(root)
                if kind=='overlap': (source/'target.c').write_text('my overlapping change\n')
                else:
                    (root/'external.c').write_text('old\n');(source/'target.c').unlink();(source/'target.c').symlink_to(root/'external.c')
                with mock.patch.object(P2P,'PINS',pins),mock.patch.object(P2P,'PACKAGE',package),self.assertRaises(ValueError):
                    P2P.prepare_source(source,root/'out')
                self.assertFalse((root/'out').exists())

    def test_wrong_boot_stops_before_running_a_program(self):
        args=mock.Mock()
        with mock.patch.object(P2P.platform,'system',return_value='Linux'),mock.patch.object(P2P.platform,'machine',return_value='x86_64'),mock.patch.object(P2P.platform,'release',return_value='unqualified'),mock.patch.object(P2P.os,'execvpe') as execute,self.assertRaises(ValueError):
            P2P.launch(args)
        execute.assert_not_called()

    def test_shared_cli_exposes_p2p_commands(self):
        result=subprocess.run([sys.executable,str(ROOT/'scripts/gpu_tool.py'),'p2p-run','--help'],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('--boot-kit',result.stdout)

    @unittest.skipUnless(shutil.which('cc'), 'C compiler is required')
    def test_actual_guard_before_and_after_cached_pci_identity(self):
        patch=(P2P.PACKAGE/'ampere-p2p-595.104.02.patch').read_text()
        section=patch.split('+++ b/src/nvidia/inc/kernel/gpu/bif/p2p_trial.h\n',1)[1].split('\n--- ',1)[0]
        header='\n'.join(s[1:] for s in section.splitlines() if s.startswith('+'))+'\n'
        self.assertEqual(sha(header.encode()),P2P.PINS['source_files']['src/nvidia/inc/kernel/gpu/bif/p2p_trial.h']['after'])
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'gpu').mkdir();(root/'os').mkdir()
            (root/'p2p_trial.h').write_text(header)
            (root/'gpu/gpu.h').write_text('#include <stdint.h>\ntypedef uint32_t NvU32; typedef int NvBool;\n#define NV_INLINE inline\n#define NV_FALSE 0\ntypedef struct OBJGPU { struct { NvU32 PCIDeviceID, PCISubDeviceID; } idInfo; void *hPci; } OBJGPU;\n')
            (root/'os/os.h').write_text('NvU32 osPciReadDword(void *, NvU32);\n')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root),str(P2P.PACKAGE/'tests/identity.c'),'-o',str(root/'test')],check=True,timeout=20)
            result=subprocess.run([str(root/'test')],capture_output=True,text=True,check=True,timeout=5)
            self.assertIn('12 lifecycle/identity checks passed',result.stdout)

if __name__=='__main__': unittest.main()
