#!/usr/bin/env python3
"""Bounded GPU inventory, PyTorch GEMM checks, and saved-run comparison."""
import argparse
import csv
import datetime
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time

SCHEMA = 1
MODES = ('fp16-fp32', 'bf16-fp32', 'fp16', 'bf16', 'fp32', 'tf32')
ENV_KEYS = ('CUDA_VISIBLE_DEVICES', 'CUDA_DEVICE_ORDER', 'HIP_VISIBLE_DEVICES',
            'ROCR_VISIBLE_DEVICES', 'ZE_AFFINITY_MASK', 'ONEAPI_DEVICE_SELECTOR',
            'NVIDIA_TF32_OVERRIDE', 'CUBLAS_WORKSPACE_CONFIG',
            'TORCH_ALLOW_TF32_CUBLAS_OVERRIDE', 'PYTORCH_ENABLE_MPS_FALLBACK',
            'PYTORCH_MPS_FAST_MATH', 'PYTORCH_MPS_PREFER_METAL', 'LD_LIBRARY_PATH')


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def save(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Reports may be updated by their owning run; public commands reject reuse.
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    tmp.replace(path)


def command(argv, timeout=12):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return dict(argv=argv, returncode=r.returncode, stdout=r.stdout[:65536], stderr=r.stderr[:8192])
    except (OSError, subprocess.TimeoutExpired) as e:
        return dict(argv=argv, error=str(e))


def inventory():
    tools = {}
    candidates = {
        'nvidia': ['nvidia-smi', '--query-gpu=index,name,uuid,pci.bus_id,driver_version,memory.total,memory.used,power.limit,temperature.gpu,clocks.sm', '--format=csv'],
        'nvidia_processes': ['nvidia-smi', '--query-compute-apps=gpu_uuid,pid,process_name,used_gpu_memory', '--format=csv'],
        'amd': ['amd-smi', 'static', '--json'],
        'amd_metrics': ['amd-smi', 'metric', '--json'],
        'rocm': ['rocm-smi', '--showproductname', '--showmeminfo', 'vram', '--showpower', '--json'],
        'intel_sycl': ['sycl-ls'],
    }
    if platform.system() == 'Linux':
        candidates['pci'] = ['lspci', '-Dnn']
    elif platform.system() == 'Darwin':
        candidates['display'] = ['system_profiler', 'SPDisplaysDataType', '-json']
    elif platform.system() == 'Windows':
        candidates['display'] = ['powershell', '-NoProfile', '-Command',
                                 'Get-CimInstance Win32_VideoController | Select-Object Name,PNPDeviceID,DriverVersion,AdapterRAM | ConvertTo-Json']
    for key, argv in candidates.items():
        if shutil.which(argv[0]):
            tools[key] = command(argv)
    nvidia = []
    raw = tools.get('nvidia', {})
    if raw.get('returncode') == 0:
        nvidia = [{k.strip(): v.strip() for k, v in row.items() if k is not None}
                  for row in csv.DictReader(io.StringIO(raw['stdout']))]
    drm = []
    for node in sorted(Path('/sys/class/drm').glob('card[0-9]*')):
        if '-' in node.name:
            continue
        item = {'node': node.name, 'device_path': str((node / 'device').resolve())}
        for field in ('vendor', 'device', 'subsystem_vendor', 'subsystem_device'):
            try:
                item[field] = (node / 'device' / field).read_text().strip()
            except OSError:
                pass
        drm.append(item)
    return dict(schema=SCHEMA, recorded_utc=stamp(), system=platform.platform(),
                python=sys.version, executable=sys.executable,
                torch_installed=importlib.util.find_spec('torch') is not None,
                environment={k: os.environ[k] for k in ENV_KEYS if k in os.environ},
                nvidia=nvidia, drm=drm, tools=tools)


def runtime_probe():
    import torch
    backends = []
    if torch.cuda.is_available():
        backends.append(dict(backend='cuda', vendor='amd' if torch.version.hip else 'nvidia',
                             devices=[dict(index=i, name=torch.cuda.get_device_name(i))
                                      for i in range(torch.cuda.device_count())]))
    if hasattr(torch, 'xpu') and torch.xpu.is_available():
        backends.append(dict(backend='xpu', vendor='intel',
                             devices=[dict(index=i, name=torch.xpu.get_device_name(i))
                                      for i in range(torch.xpu.device_count())]))
    if hasattr(torch.backends, 'mps') and torch.backends.mps.is_available():
        backends.append(dict(backend='mps', vendor='apple', devices=[dict(index=0, name='Apple MPS')]))
    return dict(torch=str(torch.__version__), cuda=torch.version.cuda, hip=torch.version.hip, backends=backends)


def unsupported(error):
    text = str(error).lower()
    return isinstance(error, (TypeError, NotImplementedError)) or any(s in text for s in
        ('not implemented for', 'not supported', 'unsupported', 'unexpected keyword', 'requires ampere'))


def worker(job):
    # Set before importing torch; do not allow MPS to silently execute on the CPU.
    if job['backend'] == 'mps':
        os.environ['PYTORCH_ENABLE_MPS_FALLBACK'] = '0'
    import torch
    backend, index = job['backend'], job['index']
    api = getattr(torch, backend)
    device = torch.device(f'{backend}:{index}' if backend != 'mps' else 'mps')
    if backend != 'mps':
        api.set_device(index)
    props = api.get_device_properties(index) if backend != 'mps' else None
    vendor = 'amd' if backend == 'cuda' and torch.version.hip else {'cuda': 'nvidia', 'xpu': 'intel', 'mps': 'apple'}[backend]
    raw_uuid = getattr(props, 'uuid', None)
    identity = dict(backend=backend, vendor=vendor, index=index,
                    name=props.name if props else 'Apple MPS',
                    uuid=str(raw_uuid) if raw_uuid else None,
                    total_memory=getattr(props, 'total_memory', None))
    capability = torch.cuda.get_device_capability(index) if vendor == 'nvidia' else None
    if capability:
        identity['compute_capability'] = list(capability)
    modes = job['modes']
    if modes == ['auto']:
        modes = ['fp16-fp32', 'fp32'] if vendor == 'nvidia' else ['fp16', 'fp32']
        if vendor == 'nvidia' and capability[0] >= 8:
            modes += ['bf16-fp32', 'tf32']
        elif vendor in ('amd', 'intel') and hasattr(api, 'is_bf16_supported'):
            try:
                if api.is_bf16_supported(including_emulation=False):
                    modes += ['bf16']
            except TypeError:
                pass  # Older runtimes cannot distinguish emulation; require an explicit mode.
    report = dict(schema=SCHEMA, status='running', recorded_utc=stamp(), device=identity,
                  framework=dict(torch=str(torch.__version__), cuda=torch.version.cuda, hip=torch.version.hip),
                  protocol={k: job[k] for k in ('size', 'samples', 'inner', 'warmup')},
                  script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), results=[])
    report['timing'] = 'cuda_events' if backend == 'cuda' else 'synchronized_host_clock'
    target = Path(job['report'])
    save(target, report)
    sync = api.synchronize
    if backend == 'cuda':
        torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
        torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False
    for mode in modes:
        a = b = c = None
        row = dict(mode=mode, status='running')
        report['results'].append(row)
        save(target, report)
        try:
            if mode in ('fp16-fp32', 'bf16-fp32', 'tf32') and vendor != 'nvidia':
                row.update(status='skipped', reason='This mode is implemented by this helper only for NVIDIA CUDA.')
                continue
            if vendor == 'nvidia' and mode in ('bf16', 'bf16-fp32', 'tf32') and capability[0] < 8:
                row.update(status='skipped', reason='Native BF16/TF32 mode requires compute capability 8+ in this helper.')
                continue
            dtype = torch.bfloat16 if mode.startswith('bf16') else torch.float16 if mode.startswith('fp16') else torch.float32
            output_dtype = torch.float32 if mode.endswith('-fp32') else dtype
            if backend == 'cuda':
                # Use the precision API available in the installed version, never mix both.
                if hasattr(torch.backends.cuda.matmul, 'fp32_precision'):
                    torch.backends.cuda.matmul.fp32_precision = 'tf32' if mode == 'tf32' else 'ieee'
                else:
                    torch.backends.cuda.matmul.allow_tf32 = mode == 'tf32'
            row['settings'] = dict(input_dtype=str(dtype), output_dtype=str(output_dtype),
                                   tf32=mode == 'tf32', reduced_precision_reduction=False if backend == 'cuda' else 'backend_default')
            def mm(a, b, out=None):
                kw = {'out_dtype': torch.float32} if mode.endswith('-fp32') else {}
                if out is not None:
                    kw['out'] = out
                return torch.mm(a, b, **kw)
            # Reference uses the same quantized inputs, evaluated on CPU in FP64.
            gen = torch.Generator(device='cpu').manual_seed(20261022)
            ah = torch.randn((96, 96), generator=gen).to(dtype)
            bh = torch.randn((96, 96), generator=gen).to(dtype)
            reference = ah.double() @ bh.double()
            got = mm(ah.to(device), bh.to(device)).float().cpu().double()
            error = (got - reference).abs()
            tolerance = .02 if dtype == torch.bfloat16 else .003 if mode == 'tf32' or dtype == torch.float16 else .00002
            relative = float(torch.linalg.vector_norm(error) / torch.linalg.vector_norm(reference).clamp_min(1e-20))
            maximum = float(error.max())
            numerical_ok = bool(torch.isfinite(got).all()) and relative <= tolerance and maximum <= tolerance * max(1., float(reference.abs().max())) * 4
            row['random_check'] = dict(passed=numerical_ok, relative_frobenius_error=relative if math.isfinite(relative) else None,
                                       max_absolute_error=maximum if math.isfinite(maximum) else None, tolerance=tolerance, seed=20261022, size=96)
            if not numerical_ok:
                raise ValueError('Seeded random numerical check failed; device testing stopped.')
            del ah, bh, reference, got, error
            n = job['size']
            required = n * n * (2 * torch.empty((), dtype=dtype).element_size() + torch.empty((), dtype=output_dtype).element_size())
            free_bytes = None
            if backend in ('cuda', 'xpu') and hasattr(api, 'mem_get_info'):
                try:
                    free_bytes = api.mem_get_info(index)[0]
                except (RuntimeError, NotImplementedError):
                    pass
            row['memory'] = dict(matrix_bytes=required, free_bytes=free_bytes)
            if free_bytes is not None and required + 256 * 1024**2 > free_bytes * .7:
                row.update(status='skipped', reason='Insufficient free VRAM with headroom; size was not reduced.')
                continue
            a = torch.ones((n, n), device=device, dtype=dtype)
            b = torch.ones_like(a)
            c = torch.empty((n, n), device=device, dtype=output_dtype)
            for _ in range(job['warmup']):
                mm(a, b, c)
            sync()
            samples = []
            for _ in range(job['samples']):
                if backend == 'cuda':
                    start, end = api.Event(enable_timing=True), api.Event(enable_timing=True)
                    start.record()
                    for _ in range(job['inner']):
                        mm(a, b, c)
                    end.record()
                    end.synchronize()
                    milliseconds = start.elapsed_time(end) / job['inner']
                else:
                    sync()
                    begin = time.perf_counter()
                    for _ in range(job['inner']):
                        mm(a, b, c)
                    sync()
                    milliseconds = (time.perf_counter() - begin) * 1000 / job['inner']
                if milliseconds <= 0 or not math.isfinite(milliseconds):
                    raise ValueError('Invalid GPU timing; device testing stopped.')
                samples.append(milliseconds)
            # Powers-of-two sizes are exactly representable in every attempted dtype.
            valid = bool(torch.isfinite(c).all().item()) and bool((c == n).all().item())
            row.update(milliseconds=samples, median_tflops=2 * n**3 / (statistics.median(samples) * 1e9),
                       min_tflops=2 * n**3 / (max(samples) * 1e9), max_tflops=2 * n**3 / (min(samples) * 1e9),
                       constant_matrix_check_passed=valid)
            if not valid:
                raise ValueError('Timed constant-matrix check failed; device testing stopped.')
            row['status'] = 'passed'
        except Exception as e:
            if unsupported(e):
                row.update(status='skipped', reason=f'{type(e).__name__}: {e}')
            else:
                row.update(status='failed', reason=f'{type(e).__name__}: {e}')
                report['status'] = 'failed'
                break
        finally:
            a = b = c = None
            if row['status'] != 'failed' and hasattr(api, 'empty_cache'):
                api.empty_cache()
            save(target, report)
    if report['status'] != 'failed':
        report['status'] = 'completed' if any(r['status'] == 'passed' for r in report['results']) else 'unsupported'
    save(target, report)
    return 0 if report['status'] == 'completed' else 1


def bench(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    before = inventory()
    save(out / 'inventory-before.json', before)
    probe = command([sys.executable, str(Path(__file__).resolve()), '_probe'], timeout=30)
    summary = dict(schema=SCHEMA, status='preparing', recorded_utc=stamp(), inventory=before, devices=[])
    save(out / 'summary.json', summary)
    save(out / 'runtime-probe.json', probe)
    if probe.get('returncode') != 0:
        summary.update(status='unavailable', error='GPU runtime discovery failed; inspect runtime-probe.json. No CPU fallback or package installation.')
        save(out / 'summary.json', summary)
        return 1
    runtime = json.loads(probe['stdout'])
    selected = next((b for b in runtime['backends'] if args.backend in ('auto', b['backend'])), None)
    if selected is None:
        summary.update(status='unavailable', error=f'No available {args.backend} GPU backend.', runtime=runtime)
        save(out / 'summary.json', summary)
        return 1
    indices = [d['index'] for d in selected['devices']] if args.devices == 'all' else list(dict.fromkeys(int(i) for i in args.devices.split(',')))
    if not indices or any(i not in [d['index'] for d in selected['devices']] for i in indices):
        raise ValueError('Requested device is absent from runtime discovery.')
    summary['status'] = 'running'
    for index in indices:
        path = out / f"{selected['backend']}-{index}.json"
        job = dict(backend=selected['backend'], index=index, modes=args.modes.split(','), size=args.size,
                   samples=args.samples, inner=args.inner, warmup=args.warmup, report=str(path))
        save(out / f'job-{index}.json', job)
        print(f"Testing {selected['backend']}:{index} (wall limit {args.timeout}s)", flush=True)
        start = time.monotonic()
        receipt = command([sys.executable, str(Path(__file__).resolve()), '_worker', json.dumps(job)], args.timeout)
        save(out / f'worker-{index}.json', receipt)
        report = json.loads(path.read_text()) if path.exists() else dict(device=job, results=[])
        if receipt.get('returncode') != 0 and not (receipt.get('returncode') == 1 and report.get('status') == 'unsupported'):
            report.update(status='failed', worker_error=receipt.get('error', receipt.get('stderr', 'worker failed')))
            for row in report.get('results', []):
                if row.get('status') == 'running':
                    row.update(status='failed', reason='Worker exited or exceeded its wall-time limit.')
            save(path, report)
        report['worker_wall_seconds'] = time.monotonic() - start
        summary['devices'].append(report)
        save(out / 'summary.json', summary)
        if report['status'] == 'failed':
            break
    save(out / 'inventory-after.json', inventory())
    statuses = [d['status'] for d in summary['devices']]
    summary['status'] = 'failed' if 'failed' in statuses else 'completed' if 'completed' in statuses else 'unsupported'
    save(out / 'summary.json', summary)
    print(json.dumps(dict(status=summary['status'], report=str(out / 'summary.json'))))
    return 0 if summary['status'] == 'completed' else 1


def device_key(d):
    ident = d['device']
    uuid = ident.get('uuid')
    return (ident['backend'], str(uuid).lower().removeprefix('gpu-')) if uuid else (ident['backend'], ident['index'], ident['name'])


def compare(args):
    if args.output.exists():
        raise FileExistsError(args.output)
    old, new = (json.loads(p.read_text()) for p in (args.before, args.after))
    if old.get('schema') != SCHEMA or new.get('schema') != SCHEMA or 'devices' not in old or 'devices' not in new:
        raise ValueError('Comparison requires summary.json files from this helper; do not relabel other protocols.')
    result = dict(schema=SCHEMA, before=str(args.before.resolve()), after=str(args.after.resolve()), comparisons=[], unmatched=[],
                  interpretation='Observed differences only; matching recorded settings does not establish causality.')
    old_devices = {device_key(d): d for d in old['devices']}
    seen = set()
    for current in new['devices']:
        key = device_key(current)
        previous = old_devices.get(key)
        if previous is None:
            result['unmatched'].append(dict(side='after', device=current['device']))
            continue
        seen.add(key)
        differences = []
        for field in ('framework', 'protocol', 'script_sha256', 'timing'):
            if previous.get(field) != current.get(field):
                differences.append(field)
        if not current['device'].get('uuid'):
            differences.append('device identity provisional: no stable UUID')
        for field in ('system', 'python', 'environment'):
            if old['inventory'].get(field) != new['inventory'].get(field):
                differences.append('inventory.' + field)
        # Compare the selected GPU's driver and cap, not dynamic temperature/clocks.
        def config(summary, dev):
            uuid = dev['device'].get('uuid', '')
            match = next((r for r in summary['inventory'].get('nvidia', []) if str(r.get('uuid', '')).lower().removeprefix('gpu-') == str(uuid).lower().removeprefix('gpu-')), None)
            return {k: match.get(k) for k in ('driver_version', 'power.limit [W]', 'pci.bus_id')} if match else None
        aconfig, bconfig = config(old, previous), config(new, current)
        if aconfig != bconfig:
            differences.append('driver/power cap/PCI identity')
        if aconfig is None or bconfig is None:
            differences.append('driver and power cap not verified by structured telemetry')
        old_modes = {r['mode']: r for r in previous['results'] if r['status'] == 'passed'}
        new_modes = {r['mode']: r for r in current['results'] if r['status'] == 'passed'}
        for mode in sorted(set(old_modes) | set(new_modes)):
            if mode not in old_modes or mode not in new_modes:
                result['unmatched'].append(dict(device=current['device'], mode=mode, side='after' if mode in new_modes else 'before'))
                continue
            a, b = old_modes[mode], new_modes[mode]
            changed = differences + ([] if a['settings'] == b['settings'] else ['precision settings'])
            result['comparisons'].append(dict(device=current['device'], mode=mode,
                before_tflops=a['median_tflops'], after_tflops=b['median_tflops'],
                change_percent=(b['median_tflops'] / a['median_tflops'] - 1) * 100,
                conditions='historical' if changed else 'recorded conditions match', differences=changed))
    for key, dev in old_devices.items():
        if key not in seen:
            result['unmatched'].append(dict(side='before', device=dev['device']))
    save(args.output, result)
    print(json.dumps(result, indent=2))
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '_probe':
        print(json.dumps(runtime_probe()))
        return 0
    if len(sys.argv) > 2 and sys.argv[1] == '_worker':
        return worker(json.loads(sys.argv[2]))
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    inv = sub.add_parser('inventory', help='Read-only inventory without GPU library dependencies')
    inv.add_argument('--output', type=Path, required=True)
    run = sub.add_parser('bench', help='Short sequential GPU compute checks using installed PyTorch')
    run.add_argument('--output', type=Path, required=True)
    run.add_argument('--backend', choices=['auto', 'cuda', 'xpu', 'mps'], default='auto')
    run.add_argument('--devices', default='all', help='Runtime indices, comma-separated, or all')
    run.add_argument('--modes', default='auto', help='auto or comma-separated: ' + ','.join(MODES))
    run.add_argument('--size', type=int, choices=[256, 512, 1024, 2048, 4096, 8192], default=4096)
    run.add_argument('--samples', type=int, choices=range(1, 11), default=5)
    run.add_argument('--inner', type=int, choices=range(1, 11), default=3)
    run.add_argument('--warmup', type=int, choices=range(1, 11), default=3)
    run.add_argument('--timeout', type=int, choices=range(5, 181), default=60)
    comp = sub.add_parser('compare', help='Compare saved helper summaries, exposing mismatched conditions')
    comp.add_argument('before', type=Path)
    comp.add_argument('after', type=Path)
    comp.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'inventory':
        if args.output.exists():
            raise FileExistsError(args.output)
        save(args.output, inventory())
        print(str(args.output.resolve()))
        return 0
    if args.command == 'compare':
        return compare(args)
    if args.modes != 'auto' and any(m not in MODES for m in args.modes.split(',')):
        parser.error('Unknown mode; see --help.')
    return bench(args)


if __name__ == '__main__':
    raise SystemExit(main())
