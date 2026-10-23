# SPDX-License-Identifier: GPL-2.0-only
"""Generate local firmware from pinned inputs; no device or boot access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tarfile

ROOT = Path(__file__).resolve().parent
PINS = json.loads((ROOT / 'provenance.json').read_text())


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check(path, expected):
    if digest(path) != expected:
        raise ValueError(f'Pinned SHA-256 mismatch: {path}')


def prepare(inputs, output, turing=False, rom=None, payload=None, fetch=False):
    firmware = ROOT / 'ampere/firmware'
    fetcher = load('unlock_fetch', firmware / 'fetch_artifacts.py')
    manifest = json.loads((firmware / 'artifacts-610.43.03.json').read_text())
    source_entry, package_entry = fetcher.inputs(manifest)
    source = fetcher.fetch(source_entry, inputs, offline=not fetch)
    package = fetcher.fetch(package_entry, inputs, offline=not fetch)
    generated = output / 'nvidia-source/src/nvidia/generated'
    generated.mkdir(parents=True)
    names = ['g_bindata_kgspGetBinArchiveBooterLoadUcode_GA102.c',
             'g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c']
    if turing:
        names += ['g_bindata_kgspGetBinArchiveBooterLoadUcode_TU102.c',
                  'g_bindata_kgspGetBinArchiveGspRmBoot_TU102.c',
                  'g_bindata_ksec2GetBinArchiveBlUcode_TU102.c']
    # Extract exact regular source members only; never extract archive paths.
    with tarfile.open(source, 'r:xz') as archive:
        for name in names:
            matches = [m for m in archive.getmembers() if m.name.endswith('/src/nvidia/generated/' + name)]
            if len(matches) != 1 or not matches[0].isfile() or matches[0].size > 16 * 1024**2:
                raise ValueError('Invalid source archive member: ' + name)
            (generated / name).write_bytes(archive.extractfile(matches[0]).read())
    decoder = load('unlock_extract', firmware / 'extract_boot_assets.py')
    generator = load('unlock_generate', firmware / 'generate_v67.py')
    amp = output / 'firmware/gpu-unlock/610.43.03'
    amp.mkdir(parents=True)
    decoder.extract_assets(output / 'nvidia-source', amp)
    gsp = generator.extract_member(package, generator.FIRMWARE_PREFIX + 'gsp_ga10x.bin')
    if hashlib.sha256(gsp).hexdigest() != generator.GSP_SHA256:
        raise ValueError('GA10x GSP hash mismatch')
    (amp / 'gsp_ga10x.bin').write_bytes(gsp)
    v67 = generator.build_payload()
    if hashlib.sha256(v67).hexdigest() != generator.PAYLOAD_SHA256:
        raise ValueError('Generated Ampere payload mismatch')
    (amp / 'v67-ga102-610.43.03.bin').write_bytes(v67)
    notice = manifest['source_artifacts'][2]['notice']
    license_data = generator.extract_member(package, notice['path'])
    if hashlib.sha256(license_data).hexdigest() != notice['sha256']:
        raise ValueError('NVIDIA license hash mismatch')
    (amp / 'NVIDIA-LICENSE').write_bytes(license_data)
    if turing:
        if rom is None:
            raise ValueError('TU104 requires the owner-supplied captured 1 MiB board ROM.')
        check(rom, PINS['turing_rom_sha256'])
        tu = output / 'firmware/gpu-unlock-tu104/610.43.03'
        tu.mkdir(parents=True)
        helper = load('unlock_turing', ROOT / 'tu104/prepare_assets.py')
        bundle, _ = helper.build_turing_bundle((generated / names[2]).read_bytes())
        (tu / 'booter_load_tu102.bin').write_bytes(bundle)
        for name in names[3:]:
            check(generated / name, PINS['turing_source_pins'][name])
        text = (generated / names[3]).read_text()
        (tu / 'gsp_boot_tu102.bin').write_bytes(decoder._array_bytes(text, 'UCODE_IMAGE')[0])
        (tu / 'gsp_boot_desc_tu102.bin').write_bytes(decoder._array_bytes(text, 'UCODE_DESC')[0])
        text = (generated / names[4]).read_text()
        (tu / 'gsp_bl_tu102.bin').write_bytes(decoder._array_bytes(text, 'UCODE_IMAGE')[0])
        (tu / 'gsp_tu10x.bin').write_bytes(generator.extract_member(package, generator.FIRMWARE_PREFIX + 'gsp_tu10x.bin'))
        if payload is None:
            payload = fetcher.fetch(PINS['turing_payload'], inputs, offline=not fetch)
        check(payload, PINS['turing_payload']['sha256'])
        shutil.copyfile(payload, tu / 'v67-tu102-610.43.03.bin')
        shutil.copyfile(rom, tu / 'board-rom-tu104.bin')
        for name, entry in PINS['turing_assets'].items():
            check(tu / name, entry['sha256'])
        (tu / 'NVIDIA-LICENSE').write_bytes(license_data)
    return output / 'firmware'
