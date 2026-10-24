#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""GPU Test: benchmark, build a qualified unlock kit, and manage a cold-boot trial."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
UNLOCK = ROOT / 'unlock'
PINS = json.loads((UNLOCK / 'provenance.json').read_text())
PCI = Path('/sys/bus/pci/devices')
AMPERE = {
    ('0x2216', '0x19da', '0x1612'): 'RTX 3080 LHR',
    ('0x2489', '0x196e', '0x1389'): 'RTX 3060 Ti LHR',
}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, data):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(data, indent=2) + '\n')


def run(argv, timeout=120, log=None):
    process = subprocess.Popen([str(a) for a in argv], text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
        if log:
            Path(log).write_text(stdout + stderr + '\nCommand exceeded its wall-time limit.\n')
        raise
    if log:
        Path(log).write_text(stdout + stderr)
    if process.returncode:
        raise RuntimeError(f'Command failed ({process.returncode}): {argv[0]}\n{(stdout + stderr)[-3000:]}')
    return stdout


def load_assets():
    spec = importlib.util.spec_from_file_location('unlock_assets', UNLOCK / 'build_assets.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def card(bdf, pci=PCI):
    if not re.fullmatch(r'[0-9a-f]{4}:[0-9a-f]{2}:[0-1][0-9a-f]\.[0-7]', bdf):
        raise ValueError('Use a full PCI address, for example 0000:2b:00.0; wildcard/all selection is not allowed.')
    path = pci / bdf
    item = dict(bdf=bdf, **{k: (path / k).read_text().strip().lower()
                           for k in ('vendor', 'device', 'subsystem_vendor', 'subsystem_device')})
    item['family'] = None
    board = (item['device'], item['subsystem_vendor'], item['subsystem_device'])
    if item['vendor'] == '0x10de' and board in AMPERE:
        item.update(family='ampere', name=AMPERE[board])
    if (bdf, item['vendor'], item['device'], item['subsystem_vendor'], item['subsystem_device']) == (
            '0000:05:00.0', '0x10de', '0x1e81', '0x3842', '0x3081'):
        item.update(family='tu104', name='EVGA RTX 2080 SUPER (matching captured ROM required)')
    if item['family'] is None:
        raise ValueError(f'{bdf}: no qualified unlock profile for these PCI/board IDs. Benchmarking remains available.')
    return item


def driver_version():
    path = Path('/sys/module/nvidia/version')
    return path.read_text().strip() if path.exists() else None


def plan(targets):
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        raise ValueError('The unlock kit is qualified only for Linux x86-64.')
    names = targets.lower().split(',')
    if not names or len(set(names)) != len(names):
        raise ValueError('Supply distinct explicit PCI addresses.')
    return dict(schema=1, kernel=platform.release(), driver=driver_version(),
                targets=[card(bdf) for bdf in names],
                qualified_kernel=PINS['tested_kernel'], qualified_driver=PINS['tested_driver'],
                action='Build and install a separate manually selected cold-boot trial; preserve normal/default boot.')


def validate_plan(data):
    if data.get('schema') != 1 or not data.get('targets'):
        raise ValueError('Invalid unlock plan.')
    if data['kernel'] != platform.release():
        raise ValueError('Kernel differs from the plan; create a fresh plan and rebuild.')
    for target in data['targets']:
        if card(target['bdf']) != target:
            raise ValueError('Selected PCI/board identity changed.')


def build(args):
    data = json.loads(args.plan.read_text())
    validate_plan(data)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    save(out / 'plan.json', data)
    header_dir = Path('/lib/modules') / data['kernel'] / 'build'
    if not header_dir.is_dir():
        raise ValueError('Matching kernel build headers are missing; nothing was installed.')
    for required in ('make', 'cc'):
        if not shutil.which(required):
            raise ValueError('Missing build dependency: ' + required)
    families = {t['family'] for t in data['targets']}
    turing = 'tu104' in families
    if turing and (args.board_rom is None or digest(args.board_rom) != PINS['turing_rom_sha256']):
        raise ValueError('TU104 needs the exact qualified owner-supplied ROM; no generic ROM or capture attempt is used.')
    for name, expected in PINS['module_sources'].items():
        if digest(UNLOCK / name) != expected:
            raise ValueError('Bundled module source changed: ' + name)
    assets = load_assets().prepare(args.inputs.resolve(), out, turing=turing,
                                   rom=args.board_rom, payload=args.turing_payload, fetch=args.fetch)
    src = out / 'source'
    shutil.copytree(UNLOCK / 'ampere', src / 'ampere', ignore=shutil.ignore_patterns('__pycache__'))
    if turing:
        shutil.copytree(UNLOCK / 'tu104', src / 'tu104', ignore=shutil.ignore_patterns('__pycache__'))
    # Upstream's tests compile parser, layout, main, and full transaction shims.
    env = dict(os.environ, GPU_UNLOCK_ASSETS=str(assets / 'gpu-unlock/610.43.03'))
    with (out / 'ampere-tests.log').open('w') as log:
        subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-v'],
                       cwd=src / 'ampere', env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    if turing:
        binary = out / 'tu104-transaction-test'
        run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-Wno-sign-compare', '-I', src / 'tu104/test-shim', '-I', src / 'tu104',
             src / 'tu104/test_transaction.c', src / 'tu104/gfw_eval.c', src / 'tu104/prep_610.c',
             src / 'tu104/layout_610.c', src / 'tu104/vbios_610.c', '-o', binary])
        harness = out / 'harness'
        harness.mkdir()
        (harness / 'assets').symlink_to(assets / 'gpu-unlock-tu104/610.43.03', target_is_directory=True)
        (harness / 'board-rom.bin').symlink_to((args.board_rom).resolve())
        with (out / 'tu104-tests.log').open('w') as log:
            subprocess.run([str(binary)], cwd=harness, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    modules = []
    for family in sorted(families):
        folder = src / 'ampere/kernel/gpu_unlock' if family == 'ampere' else src / 'tu104'
        name = 'gpu_unlock' if family == 'ampere' else 'gpu_unlock_tu104'
        run(['make', '-C', folder, f'KDIR={header_dir}', '-j4'], log=out / f'{family}-build.log')
        module = folder / (name + '.ko')
        if not run(['modinfo', '-F', 'vermagic', module]).startswith(data['kernel'] + ' '):
            raise ValueError('Built module has the wrong kernel vermagic.')
        modules.append(dict(name=name, file=str(module.relative_to(out)), sha256=digest(module)))
    run(['cc', '-O2', '-Wall', '-Wextra', UNLOCK / 'rm_issue_rate.c', '-o', out / 'rm-issue-rate'])
    files = [dict(file=str(p.relative_to(out)), sha256=digest(p)) for p in sorted(assets.rglob('*')) if p.is_file()]
    save(out / 'kit.json', dict(schema=1, status='built_and_host_tests_passed', plan=data,
         modules=modules, firmware=files, diagnostics=dict(file='rm-issue-rate', sha256=digest(out / 'rm-issue-rate')),
         qualification='Previously tested module sources; this packaged build has not undergone a new cold-boot hardware trial.'))
    print(json.dumps(dict(kit=str(out), status='built_and_host_tests_passed')))


def validate_kit(folder):
    kit = json.loads((folder / 'kit.json').read_text())
    if kit.get('schema') != 1 or kit.get('status') != 'built_and_host_tests_passed':
        raise ValueError('Kit did not complete its build and host tests.')
    validate_plan(kit['plan'])
    for entry in kit['modules'] + kit['firmware'] + [kit['diagnostics']]:
        source = (folder / entry['file']).resolve()
        if not source.is_relative_to(folder.resolve()) or digest(source) != entry['sha256']:
            raise ValueError('Kit file escaped its directory or changed: ' + entry['file'])
    return kit


def boot_entry(text, image, targets):
    lines = text.splitlines()
    for key in ('title', 'linux', 'initrd', 'options'):
        if len([l for l in lines if l.startswith(key + ' ')]) != 1:
            raise ValueError('Only plain BLS entries with one title, linux, initrd, and options line are supported.')
    if len(next(l for l in lines if l.startswith('initrd ')).split()) != 2:
        raise ValueError('Multiple initrd images on one line require a separate reviewed boot layout.')
    options = next(l for l in lines if l.startswith('options '))
    if 'gpu_unlock' in options or '$' in options or '\\' in options:
        raise ValueError('Select a normal BLS entry with explicit options and no existing unlock arguments.')
    params = []
    for family, module in [('ampere', 'gpu_unlock'), ('tu104', 'gpu_unlock_tu104')]:
        names = [t['bdf'] for t in targets if t['family'] == family]
        if names:
            params.append(module + '.targets=' + ','.join(names))
    return '\n'.join('title GPU Test - tensor unlock TRIAL (cold boot only)' if l.startswith('title ') else
                     'initrd /' + image.name if l.startswith('initrd ') else
                     l + ' ' + ' '.join(params) if l.startswith('options ') else l for l in lines) + '\n'


def secure_boot_disabled():
    if not Path('/sys/firmware/efi').exists():
        return True
    flags = list(Path('/sys/firmware/efi/efivars').glob('SecureBoot-*'))
    return len(flags) == 1 and flags[0].read_bytes()[-1:] == b'\x00'


def install(args):
    folder = args.kit.resolve()
    kit = validate_kit(folder)
    kernel = kit['plan']['kernel']
    if kernel != PINS['tested_kernel'] or driver_version() != PINS['tested_driver']:
        raise ValueError('Installer is qualified only for the documented kernel/driver pair; benchmark/build remain available.')
    base = args.base_entry.resolve()
    if base.parent != Path('/boot/loader/entries') or base.suffix != '.conf':
        raise ValueError('Select an existing normal entry under /boot/loader/entries.')
    if not re.fullmatch(r'[a-zA-Z0-9_-]{1,48}', args.name):
        raise ValueError('Invalid trial name.')
    trial_id = 'gpu-test-' + args.name
    image = Path('/boot') / f'initramfs-{kernel}-{trial_id}.img'
    entry = base.parent / (trial_id + '.conf')
    state_dir = Path('/var/lib/gpu-test') / trial_id
    module_dir = Path('/usr/lib/modules') / kernel / 'extra' / trial_id
    trial_text = boot_entry(base.read_text(), image, kit['plan']['targets'])
    if not any(l.startswith('linux ') and kernel in l for l in base.read_text().splitlines()):
        raise ValueError('Base entry does not name the kit kernel.')
    if args.dry_run:
        print(json.dumps(dict(image=str(image), entry=str(entry), state=str(state_dir / 'state.json'),
                              entry_text=trial_text, changes_default=False, reboots=False), indent=2))
        return
    if os.geteuid() != 0:
        raise PermissionError('Install creates a separate boot trial: run this explicit subcommand with sudo.')
    if not secure_boot_disabled():
        raise ValueError('Secure Boot is enabled or could not be verified; unsigned module installation is refused.')
    for path in (image, entry, state_dir, module_dir):
        if path.exists():
            raise FileExistsError(path)
    for module in kit['modules']:
        if subprocess.run(['modinfo', '-n', module['name']], capture_output=True).returncode == 0:
            raise ValueError('An unlock module is already installed; preserve that installation rather than shadowing it.')
    for tool in ('dracut', 'lsinitrd', 'depmod'):
        if not shutil.which(tool):
            raise ValueError('Required installer tool missing: ' + tool)
    state_dir.mkdir(parents=True)
    state = dict(schema=1, status='installing', kernel=kernel, plan=kit['plan'], image=str(image), entry=str(entry),
                 base_entry=str(base), base_sha256=digest(base), owned_files=[], preserved_firmware=[], module_dir=str(module_dir))
    save(state_dir / 'state.json', state)
    shutil.copyfile(base, state_dir / 'normal-entry.conf')
    try:
        module_dir.mkdir(parents=True)
        installs = [(folder / m['file'], module_dir / Path(m['file']).name) for m in kit['modules']]
        installs += [(folder / f['file'], Path('/usr/lib') / f['file']) for f in kit['firmware']]
        installs += [(folder / 'rm-issue-rate', state_dir / 'rm-issue-rate')]
        for source, destination in installs:
            if destination.exists():
                if digest(source) != digest(destination):
                    raise ValueError('Existing destination differs; refusing replacement: ' + str(destination))
                state['preserved_firmware'].append(str(destination))
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Exclusive creation prevents silently overwriting another installation.
            with destination.open('xb') as target, source.open('rb') as src:
                shutil.copyfileobj(src, target)
            destination.chmod(0o755 if destination.name == 'rm-issue-rate' else 0o644)
            state['owned_files'].append(dict(path=str(destination), sha256=digest(destination)))
            save(state_dir / 'state.json', state)
        run(['depmod', '-a', kernel])
        extra = ' '.join(str(Path('/usr/lib') / f['file']) for f in kit['firmware'])
        run(['dracut', '--kver', kernel, '--force-drivers', ' '.join(m['name'] for m in kit['modules']),
             '--install', extra, image], timeout=180, log=state_dir / 'dracut.log')
        for source, destination in installs:
            if destination.name == 'rm-issue-rate':
                continue
            extracted = subprocess.run(['lsinitrd', '-f', str(destination).lstrip('/'), str(image)], capture_output=True, timeout=30, check=True).stdout
            if hashlib.sha256(extracted).hexdigest() != digest(source):
                raise ValueError('Initramfs member verification failed: ' + str(destination))
        forced = run(['lsinitrd', '-f', 'etc/cmdline.d/20-force_drivers.conf', image])
        for m in kit['modules']:
            if 'rd.driver.pre=' + m['name'] not in forced.split():
                raise ValueError('Initramfs is missing pre-driver module ordering.')
        if digest(base) != state['base_sha256']:
            raise ValueError('Normal entry changed during installation; trial entry was not published.')
        with entry.open('x') as stream:
            stream.write(trial_text)
        state.update(status='trial_installed', image_sha256=digest(image), entry_sha256=digest(entry))
        save(state_dir / 'state.json', state)
        print(json.dumps(dict(status='trial_installed', state=str(state_dir / 'state.json'),
                              next_step='Save work, shut down fully, and manually select GPU Test - tensor unlock TRIAL. Normal default is unchanged.')))
    except BaseException:
        # Preserve the failed journal; no default boot edits or automatic retry.
        state['status'] = 'failed'
        if entry.exists() and entry.read_text() == trial_text:
            entry.unlink()
        image.unlink(missing_ok=True)
        for owned in reversed(state['owned_files']):
            path = Path(owned['path'])
            if path.is_file() and digest(path) == owned['sha256']:
                path.unlink()
        if module_dir.exists() and not any(module_dir.iterdir()):
            module_dir.rmdir()
        subprocess.run(['depmod', '-a', kernel], capture_output=True, timeout=30)
        state['rollback'] = 'Owned unchanged files removed; pre-existing firmware and normal boot preserved.'
        save(state_dir / 'state.json', state)
        raise


def verify(args):
    state = json.loads(args.state.read_text())
    validate_plan(state['plan'])
    if args.output.exists():
        raise FileExistsError(args.output)
    cmdline = Path('/proc/cmdline').read_text().split()
    logs = run(['journalctl', '-k', '-b', '--no-pager'], timeout=20)
    errors = [l for l in logs.splitlines() if re.search(r'NVRM:.*Xid|GPU has fallen off|gpu_unlock.*(?:failed|failure|error)', l, re.I)]
    receipts = []
    for target in state['plan']['targets']:
        module = 'gpu_unlock' if target['family'] == 'ampere' else 'gpu_unlock_tu104'
        selector = next((v.partition('=')[2] for v in cmdline if v.startswith(module + '.targets=')), '')
        if target['bdf'] not in selector.split(','):
            raise ValueError('Current boot does not select target ' + target['bdf'])
        if not any(target['bdf'] in line and 'unlocked' in line for line in logs.splitlines()):
            raise ValueError('Missing successful handoff log for ' + target['bdf'])
        info = (Path('/proc/driver/nvidia/gpus') / target['bdf'] / 'information').read_text()
        minor = re.search(r'Device Minor:\s*(\d+)', info)
        if minor is None:
            raise ValueError('Cannot resolve target to NVIDIA device minor.')
        helper = args.state.resolve().parent / 'rm-issue-rate'
        owned = next((e for e in state['owned_files'] if e['path'] == str(helper)), None)
        if owned is None or digest(helper) != owned['sha256']:
            raise ValueError('Installed read-only diagnostic changed.')
        rates = json.loads(run([helper, minor[1]], timeout=15))
        v1 = rates['controls']['v1']
        values = {v['name']: v['speed'] for v in v1['values']}
        passed = v1['rm_status'] == '0x00000000' and all(values.get(k) == 'full' for k in ('FMLA32', 'IMLA4'))
        receipts.append(dict(target=target, passed=passed, rates=rates))
    report = dict(status='passed' if not errors and all(r['passed'] for r in receipts) else 'failed',
                  boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(), targets=receipts, gpu_errors=errors,
                  next_step='Run the same short compute/model workload used before the trial; capability rates alone are not measured throughput.')
    save(args.output, report)
    print(json.dumps(dict(status=report['status'], report=str(args.output))))
    if report['status'] != 'passed':
        raise ValueError('Post-boot verification failed; power off and select normal boot. Do not retry warm.')


def remove(args):
    if os.geteuid() != 0:
        raise PermissionError('Removing the installed trial requires sudo.')
    path = args.state.resolve()
    if path.parent.parent != Path('/var/lib/gpu-test') or path.name != 'state.json':
        raise ValueError('Select a tool-owned state file under /var/lib/gpu-test/<trial>/state.json.')
    state = json.loads(path.read_text())
    if state.get('schema') != 1 or state.get('status') != 'trial_installed':
        raise ValueError('Only a successfully installed trial can be removed automatically.')
    image, entry = Path(state['image']), Path(state['entry'])
    trial_id = path.parent.name
    if entry != Path('/boot/loader/entries') / (trial_id + '.conf') or image != Path('/boot') / f"initramfs-{state['kernel']}-{trial_id}.img":
        raise ValueError('State paths do not match their owned trial names.')
    for item, expected in [(entry, state['entry_sha256']), (image, state['image_sha256'])]:
        if digest(item) != expected:
            raise ValueError('Trial was changed outside the tool; preserving it.')
    # Do not delete an entry selected by the saved default or one-shot boot.
    grub = shutil.which('grub2-editenv') or shutil.which('grub-editenv')
    if grub is None:
        raise ValueError('Cannot inspect saved boot selection; preserving the trial.')
    selection = run([grub, 'list'])
    if any(l.partition('=')[2] == trial_id for l in selection.splitlines() if l.startswith(('saved_entry=', 'next_entry='))):
        raise ValueError('Trial is selected by GRUB; select normal boot as the default before removing it.')
    entry.unlink()
    image.unlink()
    state.update(status='trial_removed', retained='Modules and firmware retained to avoid removing assets used by another boot entry.')
    save(path, state)
    print('Trial menu entry and image removed. Module/firmware files and normal boot were preserved. Shut down fully and boot normally to restore stock volatile GPU state.')


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ('p2p-prepare-source', 'p2p-patch-libcuda', 'p2p-run'):
        return subprocess.call([sys.executable, str(ROOT / 'scripts/gpu_p2p.py'), *sys.argv[1:]])
    if len(sys.argv) > 1 and sys.argv[1] in ('inventory', 'bench', 'compare'):
        return subprocess.call([sys.executable, str(ROOT / 'scripts/gpu_test.py'), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('inventory', 'bench', 'compare'):
        sub.add_parser(name, help='Run the benchmark helper subcommand (append --help for options).', add_help=False)
    for name in ('p2p-prepare-source', 'p2p-patch-libcuda', 'p2p-run'):
        sub.add_parser(name, help='Qualified Ampere P2P preparation/launch (append --help for options); no boot installer.', add_help=False)
    p = sub.add_parser('unlock-plan', help='Read-only qualification of explicit PCI targets')
    p.add_argument('--targets', required=True)
    p.add_argument('--output', type=Path, required=True)
    b = sub.add_parser('unlock-build', help='Build modules, generate checked assets, and run host tests; no system install')
    b.add_argument('--plan', type=Path, required=True)
    b.add_argument('--output', type=Path, required=True)
    b.add_argument('--inputs', type=Path, default=Path.home() / '.cache/gpu-test/inputs')
    b.add_argument('--fetch', action='store_true', help='Download pinned inputs when absent (about 326 MB plus Turing payload)')
    b.add_argument('--board-rom', type=Path)
    b.add_argument('--turing-payload', type=Path)
    i = sub.add_parser('unlock-install', help='Install a separate dracut/BLS cold-boot trial without changing defaults')
    i.add_argument('--kit', type=Path, required=True)
    i.add_argument('--base-entry', type=Path, required=True)
    i.add_argument('--name', default='trial')
    i.add_argument('--dry-run', action='store_true')
    v = sub.add_parser('unlock-verify', help='Read-only post-cold-boot handoff, log and issue-rate checks')
    v.add_argument('--state', type=Path, required=True)
    v.add_argument('--output', type=Path, required=True)
    r = sub.add_parser('unlock-remove', help='Remove only the owned trial entry/image; preserve shared assets')
    r.add_argument('--state', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'unlock-plan':
        if args.output.exists():
            raise FileExistsError(args.output)
        data = plan(args.targets)
        save(args.output, data)
        print(json.dumps(data, indent=2))
    elif args.command == 'unlock-build':
        build(args)
    elif args.command == 'unlock-install':
        install(args)
    elif args.command == 'unlock-verify':
        verify(args)
    else:
        remove(args)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'gpu-tool: {error}', file=sys.stderr)
        raise SystemExit(1)
