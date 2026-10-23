# Benchmark details

## Helper modes and interpretation

`gpu_test.py inventory` uses bounded read-only system tools: `nvidia-smi`, AMD SMI/ROCm SMI, `sycl-ls`, PCI enumeration on Linux, display inventory on macOS or Windows, and Linux DRM device IDs where present. Tool errors remain in the receipt. It does not require administrative privileges or import GPU libraries.

`bench` discovers PyTorch backends in a short subprocess, then benchmarks selected devices sequentially in independent subprocesses. `--backend auto` chooses the first available of CUDA/ROCm, XPU, MPS; specify a backend when several are available. Each worker has a wall-time cap including import and allocation. A timed-out worker is terminated and not retried. Driver-level hangs may outlive userspace termination: stop further work and inspect the machine; do not reset or reboot automatically.

Automatic modes:

| Runtime | Modes attempted |
| --- | --- |
| NVIDIA CUDA | FP16 inputs/FP32 output, strict FP32; BF16 inputs/FP32 output and TF32 on compute capability 8+ |
| AMD ROCm or Intel XPU | FP16 output, FP32 output; BF16 output when the runtime reports support |
| Apple MPS | FP16 output, FP32 output |

`--modes` accepts comma-separated `fp16-fp32,bf16-fp32,fp16,bf16,fp32,tf32`. Do not assume every mode exists on every card or software version. CUDA tensor modes use PyTorch `mm(out_dtype=float32)` and turn off FP16/BF16 reduced-precision reductions; older PyTorch versions may not implement this operation. Generic FP16/BF16 modes have lower-precision output and are separate results. The helper never silently changes mode, matrix size, or device to conceal unsupported hardware or out-of-memory failures.

Timing uses synchronized CUDA events for CUDA/ROCm, and synchronized host wall time for XPU/MPS. The latter includes dispatch overhead and is labeled separately. Work buffers are allocated before timing. TFLOPS are `2*N^3 / seconds / 1e12`, using dense operations, without a sparsity multiplier. Memory estimate covers the matrices with conservative headroom where runtime free-memory queries work; unknown available memory is recorded rather than guessed as free.

The correctness checks are intentionally small: a seeded 96-square random input compared with CPU FP64 using quantized inputs, plus a full result check for the timed constant matrices. They catch basic numerical failures, not comprehensive GPU correctness, arbitrary model quality, or memory stability. Record tolerances, maximum absolute error, and normalized error. TF32's tolerance differs from strict FP32. The report includes per-mode settings and script SHA for reproducibility.

The comparison helper joins stable UUIDs where available (otherwise backend/index/name), modes, and matrix sizes. It reports changes in protocol, hardware/driver inventory, library versions, visibility variables, power limits and precision settings. A provisional identity or mismatched conditions is marked historical. It does not infer that a modification caused an observed difference, and it excludes failed/skipped rows.

## Other installed runtimes

If PyTorch lacks the backend, inspect existing native tools: a reviewed cuBLAS GEMM runner for NVIDIA, rocBLAS/hipBLAS tools for AMD, oneMKL/SYCL tools for Intel, or a Metal/MPS benchmark for Apple. Verify CLI options against installed help/current primary documentation, use a finite workload and output receipt, and record math mode and synchronization. A device being listed by a driver does not prove a compute runtime can execute on it. OpenCL/Vulkan are possible alternatives when the installed application supports them; do not fabricate equivalent precision support or compare unmatched APIs as identical benchmarks.

## Inference protocol

Locate the user's actual launcher JSON/script, model files, and saved reports before asking for paths. Reuse a working bounded runner when possible. Record engine/library identity and model hash; reusing an old hash based on matching size/mtime is a metadata check, not a new cryptographic verification. Include auxiliary MTP, expert routing, control-vector, tokenizer, and vision files when these affect the profile.

For a quick test, use one fresh load, one first request, and two repeated requests, with fixed prompt, sampling and output length. Preserve existing speculative decoding if it belongs to the profile. Capture startup time, actual generated tokens, first-token latency, prompt processing, decode, reused tokens, expert-cache capacity and GPU telemetry. Keep first and repeated requests separate. Do not count a cached prompt's total tokens as newly processed tokens. An output that ends before the requested length needs a separate category or exclusion.

For a controlled before/after claim, match the number and shape of requests, engine, model tensors, precision, power cap and cache behavior. A short historical comparison may be useful when differences are disclosed. Never relabel existing CUDA ctypes results as the new PyTorch protocol, even when matrix dimensions and dtypes appear similar.

## Runtime references

Check the installed version first. Official references for the backend adapters: [AMD SMI CLI](https://rocmdocs.amd.com/projects/amdsmi/en/latest/how-to/amdsmi-cli-tool.html), [PyTorch XPU API](https://docs.pytorch.org/docs/stable/xpu.html), [MPS fallback variables](https://docs.pytorch.org/docs/stable/mps_environment_variables.html), and [PyTorch matrix multiplication](https://docs.pytorch.org/docs/stable/generated/torch.mm.html). These interfaces do not establish support for every physical GPU; the runtime probe and actual test decide availability.
