---
name: gpu-test
description: Identify GPUs, run bounded compute or model inference benchmarks, and compare saved before/after results on NVIDIA, AMD, Intel, or Apple systems using supported installed runtimes. Use for GPU performance and basic numerical checks; includes a separate explicit workflow for qualified Linux NVIDIA pre-driver unlock trials.
---

# GPU Test

Produce measured results for the user's actual GPU and workload. Discover hardware first; select an installed runtime that supports it. An unsupported backend is a reported limitation, never a CPU benchmark presented as GPU performance.

## Run a short compute test

Use the bundled Python helper (paths below are relative to this skill):

```bash
python3 scripts/gpu_test.py inventory --output /absolute/new-run/inventory.json
python3 scripts/gpu_test.py bench --output /absolute/new-run/compute
```

Inventory works without PyTorch. The benchmark uses an existing PyTorch installation: CUDA for NVIDIA, ROCm through PyTorch's `cuda` API for AMD, XPU for Intel, or MPS for Apple. Choose another installed interpreter when appropriate. Do not install large runtime packages or change drivers merely to make the helper work; inspect existing tools and use a suitable native benchmark when PyTorch is unavailable. Consult current vendor documentation for unfamiliar commands or support claims.

Defaults: all devices of the first available backend, sequential workers, 4096-square GEMMs, three warmups, five samples of three operations, and a 60-second worker timeout. Explicit selection:

```bash
python3 scripts/gpu_test.py bench --backend cuda --devices 1 --size 4096 --output /absolute/new-run/card-1
python3 scripts/gpu_test.py compare /absolute/before/summary.json /absolute/after/summary.json --output /absolute/comparison.json
```

The helper writes JSON reports, verifies a small seeded random multiplication against a CPU FP64 reference, and verifies the timed constant-matrix result. NVIDIA tensor modes use FP32 outputs with reduced-precision reduction disabled; unsupported modes are skipped with reasons. Other backends use their reported output dtype. Review failed or skipped rows, return codes, timeouts, and telemetry; a completed subprocess is not automatically a passed test. Read [benchmark details](references/benchmark-details.md) for modes, matching criteria, alternate runtimes, and inference tests.

## Preserve a useful comparison

- Check active compute workloads and available memory before launching. Leave user-owned workloads alone; select a free device or request scheduling information if needed. Keep ordinary testing bounded to minutes; stress/soak tests require that scope from the user.
- Inspect the OS GPU diagnostics for the measured time window when accessible (for example, Linux kernel Xid/device-loss messages). Save errors or state that log access was unavailable; successful math alone does not establish clean driver diagnostics.
- Use stable device UUIDs/PCI addresses where available; record device indices and visibility variables. Preserve input/output dtype, accumulation settings, dimensions, timing method, libraries, power cap, and workload. Sample temperatures and clocks without changing them.
- Keep original reports. Compare like modes and workloads, separate warm requests from first requests, and separate full prefill from reused prompt tokens. Treat mismatched builds, cache capacity, or settings as historical context. Do not attribute a combined multi-GPU speedup to one card.
- Distinguish unsupported features from broken implementations and resource failures. Stop testing a device after numerical failure, device-loss errors, or a timeout; preserve the report and inspect diagnostics before retrying. No automatic retries or escalating load.
- Matrix TFLOPS do not establish model tok/s, game FPS, memory reliability, or long-term stability. Explain exactly what was checked and its limits.

## Model inference and capability checks

When the request names a model or launcher profile, benchmark that profile using its existing runtime rather than substituting matrix math. Preserve GPU order, split, quantization, KV settings, speculation, and context limit. Use a separate localhost port and owned process group when an isolated server is needed; cap startup and requests, and clean up only owned processes. Record actual prompt/output counts and cache reuse. Configuring a 1M context is not a 1M-token ingestion test.

For NVIDIA issue-rate checks, use an already reviewed read-only diagnostic if available; labels such as FMLA32/IMLA4 are driver capability reports, not throughput measurements. Never reuse a board-specific unlock module, signed payload, register sequence, or ROM on another GPU as part of a general test. A testing request does not authorize firmware writes, overclocking, boot changes, driver replacement, or unlock attempts.

Report the GPU/model, measured workload, median plus range, correctness result, and any comparison limits; link the saved receipts. Include before/after percentages only with their matching assumptions.


## Explicitly requested GPU unlocks

Read [the unlock guide](references/unlock.md) before using `scripts/gpu_tool.py unlock-*`. It describes the PCI/ROM guards, qualified kernel/driver and dracut/BLS layout, local firmware generation, trial installation, verification, and removal. The same CLI forwards `inventory`, `bench`, and `compare` to the benchmark helper.

Use unlock actions only when the user requests them. A request to package, publish, or explain the unlocker does not request changing the current machine's boot setup. A request to benchmark does not request an unlock. Preserve already installed acceleration and avoid shadowing its modules.

For an authorized installation, select explicit PCI targets, retain a baseline, build the kit, inspect `unlock-install --dry-run`, then install within the user's authorized scope. The installer itself preserves normal/default boot. Explain the full-shutdown/manual-trial step; do not reboot automatically or load either module into a running GPU. After the user's cold boot, verify handoff and issue rates, then repeat the same benchmark. Do not generalize the TU104 ROM/board-specific module or present host tests as a fresh hardware qualification.

## Explicitly requested Ampere P2P work

Read [the P2P guide](references/p2p.md) before using `p2p-prepare-source`, `p2p-patch-libcuda`, or `p2p-run` through `scripts/gpu_tool.py`. These commands preserve installed source/libraries and boot defaults. The public package provides preparation and guarded process launch, not a generic kernel installer or default-profile promotion tool.

Preserve existing tensor modules, firmware and unrelated driver patches. Refuse overlapping target edits instead of discarding them. Keep the private libcuda restricted to the exact qualified Ampere pair; the RTX 2080 SUPER is not qualified for this P2P path. Never use NVML topology labels as proof of native BAR1 access. Require matched loaded module IDs and native RM capability before process-local mappings; verify direct remote read/write data and unchanged sources before measuring throughput. The published copy gains did not yield a consistent ESE model-speed gain. Publishing this source does not authorize changing a machine's boot profile.
