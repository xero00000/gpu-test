# Source attribution and license scope

Original `scripts/gpu_test.py`, `scripts/gpu_tool.py`, agent instructions, and project documentation are provided under the root [MIT license](LICENSE). This does not relicense bundled third-party material.

## Ampere unlock

`unlock/ampere/` is a source snapshot of [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) at commit `1799023e1d16dd1e58a7fc85823579fe93281e1a`. Its kernel module, packaging, tests, and firmware-generation scripts retain their GPL-2.0-only notices. The source and original license notices are included; no prebuilt `.ko` or firmware is bundled.

The upstream notices retain attribution to the pinned WildFlash1st CMP reference and NVIDIA source contracts. See [the retained firmware notice](unlock/ampere/firmware/NOTICE.md), [GPL-2.0 text](unlock/ampere/licenses/GPL-2.0.txt), [NVIDIA MIT notice](unlock/ampere/licenses/NVIDIA-MIT.txt), and [CMP reference notice](unlock/ampere/licenses/CMP40HX-MIT.txt).

## TU104 port and diagnostics

`unlock/tu104/` contains the board-specific TU104 port developed from that Ampere source and the Turing reference. It is provided under GPL-2.0-only, with the original source notices retained. The port was previously tested on the specific EVGA RTX 2080 SUPER profile documented in the unlock guide. Packaging changed only the offline test harness include and Python decoder import paths; the kernel module sources are unchanged.

The Turing reference is [xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock) at commit `df2fe11b370bb52cc314576d0f2f5ae47ce908f8`. Its [repository license](unlock/licenses/TURING-REFERENCE-COPYLEFT-NEXT.txt) and [EFI reference notice](unlock/licenses/TURING-EFI-MIT.txt) are preserved. `unlock/rm_issue_rate.c` retains its explicit GPL-2.0-only notice. `unlock/build_assets.py` adapts the bundled GPL asset extraction workflow and is GPL-2.0-only.

## Firmware and owner ROM

Firmware is separate from the open module source. The build recipes fetch pinned NVIDIA source/package inputs and, for TU104, a pinned reference payload, validate their hashes, and generate assets locally. The extracted NVIDIA package license is kept with the local kit and installed firmware. The root MIT license does not cover NVIDIA firmware or reference payloads.

This repository does not redistribute GPU firmware binaries, patched Booter images, compiled unlock modules, or an owner's ROM. It makes no new firmware redistribution grant. The full board ROM is supplied locally by its owner and must match the qualified hash.

[`unlock/provenance.json`](unlock/provenance.json) records exact revisions, module source hashes, derived asset hashes, and the qualified environment.
