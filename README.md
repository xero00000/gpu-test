# GPU Test

**Know what your GPU can actually do. Keep the numbers to prove it.**

A reusable agent skill for discovering GPUs, running short compute benchmarks, checking numerical results, and comparing saved before/after measurements. It also guides model inference tests using your actual launcher profile.

- Hardware inventory and available-runtime detection.
- Bounded tests, sequential device workers, and saved JSON receipts.
- Seeded numerical checks plus validation of the timed matrix result.
- Comparisons that expose changed drivers, power caps, precision, and workloads.
- NVIDIA, AMD, Intel, and Apple backend routes when a compatible runtime is installed. Hardware validation so far covers the three NVIDIA cards below.

## Real results: three cards, before and after

**141.45 → 230.13 TFLOPS of summed FP16 tensor throughput — +62.7% (88.68 additional TFLOPS).**

These are xero00000's actual measurements from 22 October 2026, comparing stock tensor-rate settings with separately applied pre-driver GPU modifications. GPU Test records performance; installing or running this skill does not unlock a GPU or produce these gains by itself.

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

Python 3.10+ is required. Inventory uses the standard library and available vendor tools. Compute tests additionally need an existing PyTorch build compatible with your GPU; the helper does not install packages or drivers.

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

This repository contains the testing skill, benchmark helper, documentation, and measured results. GPU unlock modules, firmware payloads, ROM images, and boot modifications are outside this repository. Ordinary testing leaves drivers, clocks, power caps, and boot settings unchanged.

## License

[MIT](LICENSE).
