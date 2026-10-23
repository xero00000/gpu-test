#!/usr/bin/env python3
"""Build pinned Turing-family Booter assets for offline port development only.

No GPU, /dev, /sys or boot configuration access. Not an unlock installer.
The working Ampere extractor is reused solely for NVIDIA BINDATA decoding.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

EXTRACTOR = Path(__file__).resolve().parents[1] / 'ampere/firmware/extract_boot_assets.py'
PINNED = 'e495483adf66798502667da137c4a24c55f471b86f3183020e3169ff88ad6c89'
IMAGE_HASH = 'e0f0fc93da097b5b68e5af3adb58b2235ed1fad5fce0b23592d3895af44cb743'
HEADER = (0, 256, 34048, 25088, 1, 256, 33792, 256, 0)


def decoder():
    spec = importlib.util.spec_from_file_location('ampere_bindata_decoder', EXTRACTOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module._array_bytes


def build_turing_bundle(source):
    if hashlib.sha256(source).hexdigest() != PINNED:
        raise ValueError('Turing source does not match pinned 610.43.03 input')
    decode = decoder()
    text = source.decode('ascii')
    records = {k: decode(text, k)[0] for k in (
        'IMAGE_PROD', 'HEADER_PROD', 'SIG_PROD', 'PATCH_LOC', 'PATCH_META', 'NUM_SIGS')}
    if records['HEADER_PROD'] != struct.pack('<9I', *HEADER):
        raise ValueError('unexpected Turing Booter geometry')
    if records['PATCH_META'] != struct.pack('<3I', 0, 1, 13):
        raise ValueError('unexpected Turing authentication metadata')
    if records['PATCH_LOC'] != struct.pack('<I', 0x8700):
        raise ValueError('unexpected Turing signature location')
    if records['NUM_SIGS'] != struct.pack('<I', 1) or len(records['SIG_PROD']) != 16:
        raise ValueError('unexpected Turing signature table')
    image = records['IMAGE_PROD']
    if len(image) != 0xe700 or hashlib.sha256(image).hexdigest() != IMAGE_HASH:
        raise ValueError('unexpected Turing Booter image')
    # Same container version as the original tool, separate TU102 filename.
    # Its existing GA102 loader will refuse these geometry/signature values.
    hdr = struct.pack('<14I', *HEADER, 0x8700, 13, 1, 1, 16)
    bundle = struct.pack('<II', 0x314c5442, 1) + hdr
    bundle += struct.pack('<II', len(image), 16) + image + records['SIG_PROD']
    patched = bytearray(image)
    patched[0x8700:0x8710] = records['SIG_PROD']
    return bundle, bytes(patched)


def parse_v2_descriptor(rom, offset):
    """Validate a vendor V2 FWSEC descriptor at a previously located offset.

    Locating its BIT entry and confirming board-specific loading behavior are
    separate steps. This function never searches arbitrary bytes for a match.
    """
    if offset < 0 or offset > len(rom) - 60:
        raise ValueError('truncated V2 descriptor')
    names = ('vDesc', 'StoredSize', 'UncompressedSize', 'VirtualEntry',
             'InterfaceOffset', 'IMEMPhysBase', 'IMEMLoadSize', 'IMEMVirtBase',
             'IMEMSecBase', 'IMEMSecSize', 'DMEMOffset', 'DMEMPhysBase',
             'DMEMLoadSize', 'altIMEMLoadSize', 'altDMEMLoadSize')
    d = dict(zip(names, struct.unpack_from('<15I', rom, offset)))
    size = d['vDesc'] >> 16
    if not d['vDesc'] & 1 or (d['vDesc'] >> 8) & 255 != 2 or size < 60:
        raise ValueError('unsupported FWSEC descriptor version/size')
    if size > len(rom) - offset:
        raise ValueError('descriptor extends past ROM')
    # The captured TU104 descriptor includes alternate sizes. NVIDIA's
    # s_vbiosConstructFalconUcodeWithLoader uses the primary sizes here and
    # does not select the alternate layout. Do not load alternate ranges.
    if not d['StoredSize'] or d['StoredSize'] != d['UncompressedSize']:
        raise ValueError('compressed FWSEC layout is unsupported')
    start = offset + size
    if d['StoredSize'] > len(rom) - start:
        raise ValueError('FWSEC image extends past ROM')
    for off, length in ((0, d['IMEMLoadSize']), (d['DMEMOffset'], d['DMEMLoadSize'])):
        if not length or off > d['StoredSize'] or length > d['StoredSize'] - off:
            raise ValueError('FWSEC transfer range outside stored image')
    if d['DMEMOffset'] < d['IMEMLoadSize']:
        raise ValueError('overlapping FWSEC code and data')
    if d['IMEMSecBase'] < d['IMEMVirtBase']:
        raise ValueError('FWSEC secure code starts below virtual image')
    sec = d['IMEMSecBase'] - d['IMEMVirtBase']
    if sec > d['IMEMLoadSize'] or d['IMEMSecSize'] > d['IMEMLoadSize'] - sec:
        raise ValueError('FWSEC secure code outside IMEM image')
    if d['InterfaceOffset'] >= d['DMEMLoadSize']:
        raise ValueError('FWSEC interface outside DMEM')
    d['rom_image_offset'] = start
    return d


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('nvidia_source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.nvidia_source / 'src/nvidia/generated/g_bindata_kgspGetBinArchiveBooterLoadUcode_TU102.c'
    bundle, patched = build_turing_bundle(source.read_bytes())
    args.output.mkdir(parents=True, exist_ok=True)
    assets = {'booter_load_tu102.bin': bundle, 'booter_load_tu102_sigpatched.bin': patched}
    for name, data in assets.items():
        (args.output / name).write_bytes(data)
    manifest = {'status': 'offline_development_only_not_installed',
                'source_sha256': PINNED, 'target': 'TU104 10de:1e81',
                'ucode_id': 13, 'signature_bytes': 16, 'signature_offset': 0x8700,
                'assets': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                           for name, data in assets.items()}}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
