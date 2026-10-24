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

## NVIDIA 595 Ampere P2P backport

`unlock/p2p/ampere-p2p-595.104.02.patch` adapts the BAR1 work in [aikitoria/open-gpu-kernel-modules, commit e698b05](https://github.com/aikitoria/open-gpu-kernel-modules/commit/e698b05bbe31bcff38e2ba41791dc0e5290e577c) to NVIDIA 595.104.02. The upstream commit credits geohot, nimlgen, valdemardi, ducphuc and xycjscs as co-authors. This backport retains the NVIDIA source's license scope, adds exact-board guards, uses the 595 HAL indexes, and adjusts allocation/mapping cleanup for that version. The early-identity guard and its host test are original MIT code. The upstream [COPYING notice](unlock/licenses/NVIDIA-OPEN-COPYING.txt) is retained; individual NVIDIA source files are generally MIT, and the resulting linked kernel module is dual MIT/GPLv2 as described there. This does not relicense other bundled unlock sources.

The userspace recipe was informed by the [615 patch script at c8bff540](https://github.com/aikitoria/open-gpu-kernel-modules/blob/c8bff5400e2fc8bda4cb9a141fb596aad607a35f/patch-libcuda-p2p.py). That script is not copied into this repository and does not apply unchanged to the tested 595 binary. The original MIT builder here uses the reviewed 595 offsets, checks original/candidate hashes and preserves all five branch destinations. NVIDIA's proprietary libcuda is supplied locally by its owner; neither the original nor the modified binary is redistributed or covered by this repository's MIT license.

`unlock/p2p/rm_bar1_probe.c` includes the existing GPL-2.0-only RM helper and retains that license. The CUDA copy/remote-kernel probes, Python preparation/launch tools, host tests and new documentation are original MIT work. Exact upstream revisions, patch/file hashes and userspace branch changes are recorded in [`unlock/p2p/provenance.json`](unlock/p2p/provenance.json).
