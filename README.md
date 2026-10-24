# GPU Test + Unlock

**Know what your GPU can actually do. Keep the numbers to prove it.**

An agent skill and shared command interface for discovering GPUs, benchmarking them, building supported NVIDIA unlock kits, and measuring the before/after difference. Model inference tests use your actual launcher profile.

- Hardware inventory and available-runtime detection.
- Bounded tests, sequential device workers, and saved JSON receipts.
- Seeded numerical checks plus validation of the timed matrix result.
- Comparisons that expose changed drivers, power caps, precision, and workloads.
- NVIDIA, AMD, Intel, and Apple benchmark routes when a compatible runtime is installed.
- Bundled Ampere unlock source and the board-specific EVGA TU104 port, with pinned asset generation and offline tests.
- A separate cold-boot trial installer, post-boot verification, and trial removal through the same tool.
- A source-preserving NVIDIA 595 Ampere P2P backport, private libcuda builder, guarded launcher, and measured remote-memory results.

**Benchmarking covers available supported runtimes. Unlocking is limited to the qualified Linux/NVIDIA profiles below.**

## Real results: three cards, before and after

**141.45 → 230.13 TFLOPS of summed FP16 tensor throughput — +62.7% (88.68 additional TFLOPS).**

These are xero00000's actual measurements from 22 October 2026, comparing stock tensor-rate settings with separately applied pre-driver GPU modifications. Benchmark commands record performance. The included unlock commands explicitly build and prepare a separate boot trial; installing the skill alone does not unlock a GPU or guarantee these gains.

The total is the **sum of three independently measured card medians**, with FP16 inputs and FP32 accumulation/output. It is not a measured simultaneous three-GPU workload or an ordinary FP32 shader-throughput figure.

| GPU | Power cap, unchanged | Before | After | Improvement |
| --- | ---: | ---: | ---: | ---: |
| RTX 3080 | 320 W | 59.06 TFLOPS | 94.02 TFLOPS | +59.2% |
| RTX 3060 Ti | 140 W | 36.43 TFLOPS | 63.97 TFLOPS | +75.6% |
| RTX 2080 SUPER | 160 W | 45.96 TFLOPS | 72.14 TFLOPS | +57.0% |
| **Sum of card medians** | **620 W in configured caps** | **141.45 TFLOPS** | **230.13 TFLOPS** | **+62.7%** |

620 W is the sum of configured power limits, not a measurement of combined power draw.

### Other measured tensor modes

All rows below use FP32 accumulation/output. BF16 and TF32 totals cover the RTX 3080 and RTX 3060 Ti; the RTX 2080 SUPER was not included in those modes.

| Mode | GPU | Before | After | Improvement |
| --- | --- | ---: | ---: | ---: |
| BF16 | RTX 3080 | 61.57 TFLOPS | 108.92 TFLOPS | +76.9% |
| BF16 | RTX 3060 Ti | 36.39 TFLOPS | 61.88 TFLOPS | +70.0% |
| **BF16 sum** | **Two card medians** | **97.96 TFLOPS** | **170.81 TFLOPS** | **+74.4%** |
| TF32 | RTX 3080 | 31.22 TFLOPS | 57.93 TFLOPS | +85.6% |
| TF32 | RTX 3060 Ti | 18.21 TFLOPS | 32.36 TFLOPS | +77.7% |
| **TF32 sum** | **Two card medians** | **49.43 TFLOPS** | **90.29 TFLOPS** | **+82.7%** |

FP16, BF16, and TF32 are alternative ways to use the same hardware. Their totals must not be added together.

### How these results were measured

- 8192 × 8192 × 8192 dense matrix multiplication through cuBLAS 12.8.3, using FP32 accumulation and output.
- Three warmups, followed by five samples of three GEMMs; CUDA event timing; reported throughput uses the median time per GEMM.
- Dense operation count: `2 × N³`; one TFLOPS means one trillion floating-point operations per second, with no sparsity multiplier.
- NVIDIA driver 595.104.02 and Nobara kernel 7.2.10-200.nobara.fc44.x86_64. Each card's before/after pair used the same benchmark script, library version, and configured power cap, on separate boots.
- All published constant-matrix checks passed. Output hashes matched before and after. This checks that particular calculation; it does not certify every numerical workload or long-term stability.

The historical figures above came from the original **cuBLAS ctypes runners**. The reusable helper in this repository uses **PyTorch**, defaults to size 4096, and adds a seeded random numerical check. Different protocols may produce different numbers; use the same protocol for a new before/after comparison.

**Receipts:** [all measurements and totals](results/2026-10-22/comparison.json), [six before/after receipt hashes](results/2026-10-22/manifest.json), and [skill validation](results/2026-10-22/skill-validation.json). The published receipts retain timing samples and output hashes; local machine paths, boot arguments, GPU UUIDs, and PCI addresses were omitted. Original receipt hashes are included for provenance.

Matrix throughput is distinct from model tokens per second and game frame rates. Multi-GPU inference depends on memory movement, expert caching, quantization, speculation, and the rest of the workload.

## One command interface

```bash
python3 scripts/gpu_tool.py inventory --output runs/inventory.json
python3 scripts/gpu_tool.py bench --output runs/before
python3 scripts/gpu_tool.py --help
```

For a supported unlock, start with an explicit card selection. Replace the example address with your card's PCI address:

```bash
python3 scripts/gpu_tool.py unlock-plan --targets 0000:2b:00.0 --output runs/unlock-plan.json
python3 scripts/gpu_tool.py unlock-build --plan runs/unlock-plan.json --output runs/unlock-kit --fetch
```

The build generates firmware locally from pinned, hash-checked inputs, compiles matching kernel modules, and runs the host tests. It downloads about 326 MB when inputs are absent. Python 3.11+, a C compiler, make, matching kernel headers, and either `zstd` or Python's zstd support are needed. No driver package is installed.

Review the proposed trial using your existing **normal** BLS entry:

```bash
sudo python3 scripts/gpu_tool.py unlock-install --kit runs/unlock-kit --base-entry /boot/loader/entries/NORMAL.conf --dry-run
# Use the same command without --dry-run to install the separate trial.
```

The installer preserves the normal boot entry and default, verifies the new initramfs members, and does not reboot or hot-load a module. Save work, shut down fully, and manually select the trial entry. Then:

```bash
sudo python3 scripts/gpu_tool.py unlock-verify --state /var/lib/gpu-test/gpu-test-trial/state.json --output runs/unlock-verification.json
python3 scripts/gpu_tool.py bench --output runs/after
python3 scripts/gpu_tool.py compare runs/before/summary.json runs/after/summary.json --output runs/comparison.json
```

Use the same benchmark options before and after. For trial removal:

```bash
sudo python3 scripts/gpu_tool.py unlock-remove --state /var/lib/gpu-test/gpu-test-trial/state.json
```

Removal checks ownership and hashes and preserves module/firmware files that another boot entry might use. A trial selected as the saved GRUB default or next boot is preserved until that selection is changed.

### Qualified unlock profiles

| Card/profile | Selection limits | Installation scope |
| --- | --- | --- |
| RTX 3080 LHR | PCI `10de:2216`, subsystem `19da:1612`; explicit PCI address | Ampere module |
| RTX 3060 Ti LHR | PCI `10de:2489`, subsystem `196e:1389`; explicit PCI address | Ampere module |
| EVGA RTX 2080 SUPER | PCI `10de:1e81`, subsystem `3842:3081`, address `0000:05:00.0`, exact captured ROM hash | Separate TU104 module |

The boot installer currently accepts the tested **Linux x86-64, kernel 7.2.10-200.nobara.fc44.x86_64, NVIDIA 595.104.02, dracut + plain BLS** setup with Secure Boot disabled. Builds on another kernel do not establish qualification, and the installer refuses that environment. Existing unlock installations are preserved rather than shadowed. Other GPU families can still be benchmarked; they have no unlock path here.

The TU104 path additionally requires `--board-rom /path/to/your/captured-board.rom` during `unlock-build`. The owner supplies it; the tool checks the qualified SHA-256 and does not capture or distribute a ROM. GPU flash writes and automatic resets/reboots are not part of this tool.

The module sources were exercised in the earlier hardware trials behind the results below. The new package passed module builds, host transaction tests, and an installer dry run; **the newly packaged installer has not undergone a fresh cold-boot hardware trial**. Full support details, rollback behavior, source attribution, and firmware handling are in [the unlock guide](references/unlock.md). [Package validation receipt](results/unlock-package-validation.json).

## Ampere P2P: RTX 3060 Ti ↔ RTX 3080

The 24 October 2026 cold-boot trial passed **actual GPU remote reads and writes in both directions**, while retaining the existing tensor unlocks on all three cards. It requires both the kernel BAR1 backport and a private, per-process libcuda patch for the tested 595.104.02 binary.

| 16 MiB runtime copy | Installed libcuda | Private libcuda | Change |
| --- | ---: | ---: | ---: |
| RTX 3060 Ti → RTX 3080 | 2.647 GB/s | 3.263 GB/s | +23.3% |
| RTX 3080 → RTX 3060 Ti | 2.468 GB/s | 2.949 GB/s | +19.5% |

Four seeded remote read/write kernels and 24 copy cases per library passed. All three tensor unlocks retained full reported issue rates, and seven matrix checks passed. The matched experimental ESE profile produced identical tokens, with **no consistent model-speed gain**: short-prompt median decode changed +1.3%, and 128-token-prompt decode changed −2.2%. Small changes from three repetitions with the desktop active are not established performance gains.

Prepare private working copies without changing the installed source, libraries or boot configuration:

```bash
python3 scripts/gpu_tool.py p2p-prepare-source --source /usr/src/nvidia-595.104.02 --output runs/p2p-source
python3 scripts/gpu_tool.py p2p-patch-libcuda --source /usr/lib64/libcuda.so.595.104.02 --output runs/p2p-libcuda
```

Source preparation retains unrelated existing patches and refuses overlapping target changes. The libcuda builder accepts one exact binary hash, preserves branch destinations, and refuses existing output. No NVIDIA library binary, initramfs, firmware or compiled module is distributed.

**This is a qualified case study and preparation/verification workflow, not a general P2P installer.** The guarded `p2p-run` command additionally requires an explicit boot-module manifest, exact tested board identities/topology, native RM BAR1 support, and the reviewed private library. The RTX 2080 SUPER is excluded from the P2P process; its existing tensor unlock is preserved. NVML topology labels alone are insufficient proof.

See the [P2P guide, build boundaries, launch command and default-profile recipe](references/p2p.md), [pinned patch provenance](unlock/p2p/provenance.json), and [sanitized measurements](results/2026-10-24-p2p/verification.json). The public preparation tools were tested against the local source/library and the launcher was checked on the already-qualified boot; this publication did not perform another cold boot or install a replacement driver.

## Install as an agent skill

Clone into your Codex skills directory (the destination must not already exist):

```bash
git clone https://github.com/xero00000/gpu-test.git "${CODEX_HOME:-$HOME/.codex}/skills/gpu-test"
```

Then ask your agent:

```text
$gpu-test identify my GPUs and run a short benchmark
$gpu-test compare my saved before and after results
$gpu-test benchmark my existing model launcher profile
```

[SKILL.md](SKILL.md) is the agent entry point. The skill can also be used by other agents that support this format.

## Run the helper directly

Python 3.10+ is required for the benchmark helper; unlock preparation requires Python 3.11+. Inventory uses the standard library and available vendor tools. Compute tests additionally need an existing PyTorch build compatible with your GPU; the helper does not install packages or drivers.

```bash
# Run from this repository. Each output path must be new.
python3 scripts/gpu_test.py inventory --output runs/inventory.json
python3 scripts/gpu_test.py bench --output runs/before

# Capture a later measurement using the same settings.
python3 scripts/gpu_test.py bench --output runs/after
python3 scripts/gpu_test.py compare runs/before/summary.json runs/after/summary.json --output runs/comparison.json
```

Select a card and workload explicitly when needed:

```bash
python3 scripts/gpu_test.py bench --backend cuda --devices 1 --size 4096 --output runs/card-1
```

Defaults are all devices of the first available backend, sequential workers, three warmups, five samples of three GEMMs, and a 60-second limit per worker. Device indices come from the selected runtime and may be affected by visibility variables. Existing output is preserved; failed and unsupported modes remain visible in the reports.

| Backend | Runtime route | Validation status |
| --- | --- | --- |
| NVIDIA | PyTorch CUDA | Tested on RTX 3080, RTX 3060 Ti, RTX 2080 SUPER |
| AMD | PyTorch ROCm through its CUDA API | Adapter included; no local hardware validation |
| Intel | PyTorch XPU | Adapter included; no local hardware validation |
| Apple | PyTorch MPS | Adapter included; no local hardware validation |

Support depends on the card, OS, installed libraries, and math mode. The helper reports unavailable backends and unsupported modes; it does not substitute CPU timing for GPU results. [Benchmark details and inference guidance](references/benchmark-details.md) explain precision, correctness limits, and alternate runtimes.

The published helper passed ten supported GPU/mode combinations on this machine, including numerical checks. Validation also checked unsupported Turing modes, an unavailable backend, comparison arithmetic, changed-setting detection, and preservation of existing reports.

## Scope

This repository contains the skill, shared CLI, benchmark helper, unlock module source, local asset generators, P2P source/library preparation, host tests, and measured results. Generated firmware, ROM images, CUDA libraries, compiled modules, initramfs images and local kits are excluded from Git. Benchmark commands leave driver, clock, power-cap, and boot settings unchanged; explicit unlock installation creates only its separate trial and supporting files. P2P preparation creates private copies, and `p2p-run` selects the private library only for the explicitly launched process.

## License

Original benchmark/CLI code and documentation: [MIT](LICENSE). Bundled unlock sources and derived asset generators: GPL-2.0-only with retained notices; downloaded firmware has separate terms. See [third-party attribution](THIRD_PARTY.md).
