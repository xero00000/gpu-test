#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare the qualified 595 Ampere P2P backport and a private libcuda copy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'unlock/p2p'
PINS = json.loads((PACKAGE / 'provenance.json').read_text())

def require(condition, message):
    if not condition:
        raise ValueError(message)

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(8 * 1024**2), b''):
            h.update(block)
    return h.hexdigest()

def save(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')

def private_output(path):
    path = path.expanduser().resolve()
    for system in ('/boot', '/etc', '/usr', '/var', '/opt', '/lib', '/lib64', '/sys', '/proc', '/dev'):
        require(not path.is_relative_to(system), 'Use a new private working directory, outside system paths.')
    require(not path.exists(), 'Output already exists; original artifacts are preserved.')
    return path

def snapshot(folder):
    return {str(p.relative_to(folder)): {'symlink': os.readlink(p)} if p.is_symlink() else {'sha256': digest(p)}
            for p in sorted(folder.rglob('*')) if p.is_symlink() or p.is_file()}

def prepare_source(source, output):
    source = source.expanduser().resolve()
    out = private_output(output)
    require(source.is_dir(), 'Source directory is missing.')
    require(not out.is_relative_to(source) and not source.is_relative_to(out), 'Keep output separate from the input source tree.')
    require(re.search(r'^NVIDIA_VERSION\s*=\s*595\.104\.02\s*$', (source / 'version.mk').read_text(), re.M), 'Only NVIDIA 595.104.02 source is supported.')
    patch = PACKAGE / 'ampere-p2p-595.104.02.patch'
    require(digest(patch) == PINS['patch_sha256'], 'Bundled patch hash differs.')
    for name, hashes in PINS['source_files'].items():
        path = source / name
        require(not any(p.is_symlink() for p in [path, *path.parents] if p != source.parent), 'Patch targets must not traverse symlinks: ' + name)
        if hashes['before'] is None:
            require(not path.exists(), 'Patch target already exists: ' + name)
        else:
            require(path.is_file() and digest(path) == hashes['before'], 'A patch target differs; preserve and review the overlapping edit: ' + name)
    require(shutil.which('patch'), 'The patch command is required.')
    before = snapshot(source)
    out.mkdir(parents=True)
    copy = out / 'source'
    shutil.copytree(source, copy, symlinks=True)
    save(out / 'input-files.json', before)
    try:
        require(snapshot(copy) == before, 'Source copy differs from the input.')
        command = ['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i', str(patch)]
        for dry in [True, False]:
            process = subprocess.run([*command, *(['--dry-run'] if dry else [])], cwd=copy,
                                     text=True, capture_output=True, timeout=60)
            (out / ('patch-check.log' if dry else 'patch.log')).write_text(process.stdout + process.stderr)
            require(process.returncode == 0, 'Patch did not apply cleanly; input tree was left untouched.')
        after = snapshot(copy)
        targets = set(PINS['source_files'])
        require(all(before[name] == after.get(name) for name in before if name not in targets), 'An unrelated edit was lost.')
        require(set(after) - set(before) == {name for name, pins in PINS['source_files'].items() if pins['before'] is None}, 'Unexpected files appeared in the private copy.')
        require(all(digest(copy / name) == pins['after'] for name, pins in PINS['source_files'].items()), 'Patched source differs from the hardware-tested target files.')
        require(snapshot(source) == before, 'Input source changed during preparation.')
        result = dict(status='prepared_not_built_or_installed', driver=PINS['driver'], source=str(source),
                      patched_source=str(copy), input_files=len(before), modified_files=sorted(targets),
                      unrelated_files_preserved=len(set(before) - targets), original_unchanged=True,
                      patch_sha256=PINS['patch_sha256'])
        save(out / 'preparation.json', result)
        return result
    except Exception as error:
        save(out / 'failure.json', dict(status='stopped', reason=str(error), input_source=str(source)))
        raise

def patch_bytes(original):
    pins = PINS['libcuda']
    require(hashlib.sha256(original).hexdigest() == pins['source_sha256'], 'Unrecognized libcuda binary; no signature scanning or guessed offsets are used.')
    data = bytearray(original)
    for site in pins['changes']:
        offset = site['offset']
        old, new = bytes.fromhex(site['before']), bytes.fromhex(site['after'])
        require(len(old) == len(new) and data[offset:offset+len(old)] == old, 'Original branch bytes differ: ' + site['name'])
        target = offset + len(old) + int.from_bytes(old[1:] if len(old) == 2 else old[2:], 'little', signed=True)
        replacement = offset + (2 if len(new) == 2 else 5) + int.from_bytes(new[1:] if len(new) == 2 else new[1:5], 'little', signed=True)
        require(target == replacement == site['target'], 'Branch target changed: ' + site['name'])
        data[offset:offset+len(old)] = new
    require(hashlib.sha256(data).hexdigest() == pins['candidate_sha256'], 'Private candidate hash differs.')
    return data

def patch_library(source, output):
    source = source.expanduser().resolve()
    out = private_output(output)
    candidate = patch_bytes(source.read_bytes())
    out.mkdir(parents=True)
    name = 'libcuda.so.595.104.02'
    (out / name).write_bytes(candidate)
    (out / name).chmod(0o755)
    for link in ['libcuda.so', 'libcuda.so.1']:
        (out / link).symlink_to(name)
    require(digest(source) == PINS['libcuda']['source_sha256'], 'Input library changed during copying.')
    manifest = dict(driver=PINS['driver'], source=str(source), candidate=str(out / name),
                    **PINS['libcuda'], scope='Private process library for the cold-boot-qualified Ampere pair only; no system installation.')
    save(out / 'manifest.json', manifest)
    return manifest

def launch(args):
    require(platform.system() == 'Linux' and platform.machine() == 'x86_64', 'Only the qualified Linux x86-64 case is supported.')
    require(platform.release() == PINS['kernel'], 'Wrong kernel.')
    require(Path('/sys/module/nvidia/version').read_text().strip() == PINS['driver'], 'Wrong installed driver.')
    kit = json.loads(args.boot_kit.read_text())
    require(kit['kernel'] == PINS['kernel'] and kit['driver'] == PINS['driver'], 'Boot manifest has the wrong qualification.')
    for name in ['nvidia', 'nvidia_uvm', 'nvidia_modeset', 'nvidia_drm']:
        b = Path('/sys/module', name, 'notes/.note.gnu.build-id').read_bytes()
        n, size, kind = struct.unpack_from('<III', b)
        start = 12 + ((n + 3) & ~3)
        require(kind == 3 and b[start:start+size].hex() == kit['built_modules'][name.replace('_', '-')]['build_id'], 'Loaded driver differs from the explicit boot manifest: ' + name)
    # This launcher deliberately retains the tested topology, like the TU104 profile.
    boards = [('0000:04:00.0', '0x2489', '0x196e', '0x1389'), ('0000:2b:00.0', '0x2216', '0x19da', '0x1612')]
    for bdf, device, subvendor, subdevice in boards:
        root = Path('/sys/bus/pci/devices') / bdf
        for key, value in [('vendor', '0x10de'), ('device', device), ('subsystem_vendor', subvendor), ('subsystem_device', subdevice)]:
            require((root / key).read_text().strip().lower() == value, 'Unqualified board: ' + bdf)
    probe = args.rm_probe.resolve()
    require(digest(probe) == args.rm_probe_sha256, 'GET-only diagnostic executable hash differs.')
    output = subprocess.check_output([str(probe)], text=True, timeout=15)
    rows = [json.loads(line) for line in output.splitlines()]
    require(any(x.get('ids') == [0x400, 0x2b00] and x.get('bar1_supported') and x.get('os_error') == 0 and x.get('rm_status') == 0 for x in rows), 'Native BAR1 capability is unavailable.')
    private = args.library.resolve()
    manifest = json.loads((private / 'manifest.json').read_text())
    require(digest(manifest['source']) == PINS['libcuda']['source_sha256'], 'Installed libcuda changed.')
    candidate = private / 'libcuda.so.595.104.02'
    require(digest(candidate) == PINS['libcuda']['candidate_sha256'], 'Private libcuda differs from the reviewed candidate.')
    for name in ['libcuda.so', 'libcuda.so.1']:
        require((private / name).resolve() == candidate, 'Private library symlink differs.')
    inventory = subprocess.check_output(['nvidia-smi', '--query-gpu=pci.bus_id,uuid', '--format=csv,noheader'], text=True, timeout=15)
    uuids = {}
    for line in inventory.splitlines():
        bus, uuid = [x.strip() for x in line.split(',')]
        if bus.lower().startswith('00000000:'):
            bus = bus[4:]
        require(uuid.startswith('GPU-') and re.fullmatch(r'GPU-[0-9a-fA-F-]+', uuid), 'Unexpected GPU UUID format.')
        uuids[bus.lower()] = uuid
    selection = [uuids[bdf] for bdf, *_ in boards]
    command = args.program[1:] if args.program[:1] == ['--'] else args.program
    require(bool(command), 'Supply -- COMMAND [ARGS...].')
    env = os.environ.copy()
    env.update(CUDA_DEVICE_ORDER='PCI_BUS_ID', CUDA_VISIBLE_DEVICES=','.join(selection), GPU_P2P_PRIVATE_LIBCUDA='595.104.02-ampere-r2')
    env['LD_LIBRARY_PATH'] = str(private) + (os.pathsep + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    os.execvpe(command[0], command, env)

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('p2p-prepare-source', help='Copy 595 source, preserve unrelated edits and apply the exact backport; no build or install')
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('p2p-patch-libcuda', help='Create a new private copy of the exact reviewed 595 binary; no system writes')
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p = sub.add_parser('p2p-run', help='Guarded application launch for the tested topology after separate cold-boot qualification')
    p.add_argument('--boot-kit', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--rm-probe', type=Path, required=True)
    p.add_argument('--rm-probe-sha256', required=True)
    p.add_argument('program', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.action == 'p2p-prepare-source':
        print(json.dumps(prepare_source(args.source, args.output), indent=2))
    elif args.action == 'p2p-patch-libcuda':
        print(json.dumps(patch_library(args.source, args.output), indent=2))
    else:
        launch(args)
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print('gpu-p2p: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
