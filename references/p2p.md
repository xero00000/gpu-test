# Qualified Ampere P2P on NVIDIA 595.104.02

This packages the 24 October 2026 r2 backport and measurements. It preserves the existing tensor unlocks and driver-source edits. Preparation creates new private directories; no command in this P2P package installs a driver, changes boot defaults, resets a GPU, flashes firmware or rebuilds a host initramfs.

## What was actually qualified

| Component | Tested configuration |
| --- | --- |
| OS/kernel | Linux x86-64, Nobara `7.2.10-200.nobara.fc44.x86_64` |
| Driver | NVIDIA open modules `595.104.02` |
| Ampere pair | RTX 3060 Ti `10de:2489`, subsystem `196e:1389`, PCI `0000:04:00.0`; RTX 3080 `10de:2216`, subsystem `19da:1612`, PCI `0000:2b:00.0` |
| Other card | EVGA RTX 2080 SUPER retains its existing tensor unlock; excluded from the private P2P runtime |
| Platform | Existing large BAR1 windows; IOMMU passthrough (`iommu=pt`), confirmed after boot |
| Link bottleneck | 3060 Ti PCIe Gen3 x4; 3080 Gen4 x16 |
| CUDA probes | Toolkit/runtime 13.4, driver API reports 13.2 |

Different boards, topologies, kernels, driver binaries or BAR layouts require separate investigation and qualification. No CMP 170HX P2P result is claimed. The board guard does not write PCI configuration; it reads identification dwords only if cached IDs are unavailable during early KernelBif construction. Twelve host checks cover cached and early identities, missing handles, wrong subsystem IDs and inaccessible PCI data.

The actual cold-boot trial retained all 37 existing driver-source differences and 14 unlock module/firmware assets. Static BAR1 became available on the Ampere cards. The installed CUDA library still rejected GA104/GA102 because internal device identifiers differ; five reviewed branch changes in a private copy resolved the userspace gate. Other capability/address-space/RM checks remain in that binary. The modified library has eight actual byte differences and unchanged branch targets. It is not a generic mixed-generation enablement patch.

The upstream 615 kernel changes were selectively backported; upstream install, UVM, registry, generic capability and IO-VAS changes were not applied wholesale. NVIDIA 595 uses HAL indexes 43/45 for GA102/GA104. The backport keeps the original Turing HAL/static-BAR decision and adds allocation/PTE cleanup checks. The actual patch and all pre/post target-file hashes are in [`unlock/p2p/`](../unlock/p2p/).

## Prepare source while retaining old patches

Use the already-installed, already-patched NVIDIA source as input, not a replacement stock tree:

```bash
python3 scripts/gpu_tool.py p2p-prepare-source \
  --source /usr/src/nvidia-595.104.02 --output runs/p2p-source
```

Requires Python 3.11+ and `patch`. The helper verifies the patch and all target-file preimages, copies the complete input tree, applies with zero fuzz, verifies every patched target against the hardware-tested postimage, and checks all unrelated files and the original input again. It refuses a modified target rather than replacing an existing overlapping patch. Existing output is refused. `preparation.json` records what happened; `input-files.json` retains local file hashes. Neither receipt should be uploaded without reviewing local paths.

This prepares source only. With matching kernel headers and build dependencies already installed, an explicit local build is:

```bash
make -C runs/p2p-source/source -j4 modules
```

Do not run `modules_install` over the working modules as part of this recipe. The hardware trial used a separate dracut module root and image, preserving original host module files. A qualified boot integration must:

1. Keep the working tensor image/entry and saved default intact for the trial.
2. Use a private copy of the matching module tree and replace NVIDIA modules only there. Retain the existing Ampere/TU104 unlock modules and their exact firmware assets.
3. Build a separate initramfs with pre-driver order: `gpu_unlock`, `gpu_unlock_tu104`, then NVIDIA modules. Retain each existing explicit target selection.
4. Extract both images and compare unlock module/firmware bytes. Check private NVIDIA module hashes/build IDs, module resolution, pre-driver order and options.
5. Publish an independent plain BLS entry with the retained root/kernel/unlock options and platform settings under review. Keep its normal/default entry available.
6. Fully shut down, power on and manually select the trial. Verify loaded GNU build IDs, handoff logs, tensor issue rates and kernel diagnostics before peer mappings.

The existing `unlock-install` handles tensor-only kits. It does **not** build this P2P driver/image or accept an already-unlocked boot as a P2P base. This publication includes no generic P2P boot installer; the original bespoke local image integration was the hardware-tested path. Compiling the public sources alone is not a new cold-boot qualification.

## Make the private CUDA copy

```bash
python3 scripts/gpu_tool.py p2p-patch-libcuda \
  --source /usr/lib64/libcuda.so.595.104.02 --output runs/p2p-libcuda
```

The source must match SHA-256 `6fad8879f66fdece9570849fb86f9022e8023ddd15f5585177e3a0da7558114c`. The candidate must match `0ee57db9848eb3389587866a4b2bba63ae86a9535cc3ffb005885bf882d1766d`. The helper refuses other binaries, verifies original bytes and branch destinations, and writes only the new private directory with local symlinks and a manifest. It never scans for approximate signatures or overwrites an installed library. Existing bytes outside the reviewed changes are retained.

No `.so`, firmware, ROM, `.ko`, model or initramfs binary is included in Git. The owner's local library remains subject to its own license.

## Verification source and guarded application launch

With the already-installed CUDA toolkit and a C/C++ compiler:

```bash
mkdir -p runs/probes
cc -O2 -Wall -Wextra -Werror unlock/p2p/rm_bar1_probe.c -o runs/probes/rm-bar1-probe
g++ -O2 -std=c++17 -Wall -Wextra -Werror=format \
  -I/usr/local/cuda/include unlock/p2p/peer_kernel_probe.cpp \
  -L/usr/local/cuda/lib64 -Wl,-rpath,/usr/local/cuda/lib64 \
  -lcudart -lcuda -o runs/probes/peer-kernel
g++ -O2 -std=c++17 -Wall -Wextra -Werror=format \
  -I/usr/local/cuda/include unlock/p2p/copy_probe.cpp \
  -L/usr/local/cuda/lib64 -Wl,-rpath,/usr/local/cuda/lib64 \
  -lcudart -o runs/probes/copy-probe
```

`rm_bar1_probe.c` uses GET-only RM controls, including ABI size checks. It intentionally expects the tested three-attached-GPU case. It reports native pair capabilities and per-device static BAR1, without enabling mappings. NVML `topo -p2p` can report broader OK pairs, including Turing, even when native BAR1 is unavailable. Its labels are not sufficient evidence.

After the separate cold boot has been qualified, pass an explicit module manifest and the hash of the locally built GET diagnostic:

```bash
p2p_probe_sha="$(sha256sum runs/probes/rm-bar1-probe | cut -d' ' -f1)"
python3 scripts/gpu_tool.py p2p-run \
  --boot-kit unlock/p2p/tested-modules.json \
  --library runs/p2p-libcuda \
  --rm-probe runs/probes/rm-bar1-probe --rm-probe-sha256 "$p2p_probe_sha" \
  -- runs/probes/peer-kernel
```

The included `tested-modules.json` contains the **actual original r2 build IDs**. It works only with that loaded build. A separately rebuilt driver needs its own manifest (`kernel`, `driver`, and `built_modules.NAME.build_id`, with hyphenated NVIDIA module names), captured from its private build outputs and independently qualified after cold boot. Do not copy the live stock driver's IDs just to get past this check.

The launcher checks kernel/driver, four loaded module IDs, exact PCI board IDs and tested topology, the GET diagnostic hash, native RM BAR1 capability, and installed/private library hashes and symlinks. UUIDs are resolved locally with `nvidia-smi`; no owner UUIDs are shipped. It then exposes only the two Ampere GPUs and selects the private library for the requested process. Applications must retain that selection. TU104 remains available through ordinary launches with its original tensor unlock.

The remote-kernel probe checks 262,144 bytes per case with deterministic seeded inputs and unchanged source buffers: read and write from each GPU. The copy probe tests 4 KiB, 64 KiB and 16 MiB transfers, one warmup and five samples, capped at 16 MiB each for source/destination per GPU. After the kernel checks pass, the same guarded command can launch `runs/probes/copy-probe --enable-ampere-peer` or an actual compatible application. The two probe binaries are qualified for the documented topology, not arbitrary devices. Check active workloads, host/GPU memory and diagnostics, apply a finite timeout, and stop after a numerical/driver failure or timeout.

## Optional all-mods default profile

On the original machine, the verified image was subsequently selected by a new BLS entry titled **Nobara - GPU All Mods (3-GPU tensor + Ampere P2P)**. Its boot settings and image were unchanged from the qualified r2 boot. The original tensor-only entry and standard/rescue entries were retained, and private userspace files were installed to a permanent owner-controlled directory.

That machine's root UUID, GPU UUIDs, private paths and image are intentionally not published. The new default boot enables the kernel/tensor parts; applications use the guarded private-library launcher for P2P. It does not inject the private library into every application. The qualified TU104 tensor path is preserved, rather than exposing TU104 to the private userspace P2P patch.

For another authorized deployment, first create and independently verify a separate cold-boot trial. Only after that may an explicitly requested default-profile change copy the tested BLS settings under a new title/ID and select that ID with the local GRUB tools. Back up the old saved entry/state, keep its image available, verify the resolved default and both image hashes, and retain a command to restore the prior default. No public command here performs that promotion automatically. A future kernel/driver update requires a matching new qualification.

## Measurements and validation limits

[Sanitized receipts](../results/2026-10-24-p2p/manifest.json) retain transfer ranges, tensor timing samples, ESE request timings, issue-rate values and original receipt hashes. They omit host paths, UUIDs and boot arguments.

The 16 MiB matched runtime-copy medians improved 23.3% toward the 3080 and 19.5% toward the 3060 Ti. Four direct kernels and 24 copy cases per library passed. All nine v1 issue-rate groups on each of three GPUs remained zero/full; v2 retained nine Ampere groups and eight TU104 groups. Seven 4096-square constant-matrix checks passed; these are bounded math checks, not a general accuracy or sustained-stability certification. No NVIDIA Xid, GPU-loss or PCIe Bus Error appeared in captured logs.

The same experimental ESE binary/model/profile ran six requests per library (three repeats each of 17- and 128-token prompts; 32 output tokens each). Median decode changed `6.098 → 6.176` tokens/s and `6.159 → 6.022` tokens/s. Generated token IDs were identical between passes, across repeats and against the original benchmark; the harness checked finite logits every step. No consistent model-speed benefit was established. The installed-library pass ran first; desktop/load/cache effects were not isolated. The experimental model's research architecture gate does not imply public ESE Studio support.

Host tests cover exact branch targets, unknown binaries, conflicting source patches, symlink escapes, output preservation, wrong-boot refusal and the actual early-identity helper. Public preparation reproduced the tested target-source hashes and candidate library hash. The published launcher passed a read-only capability/library-selection check on the already-qualified boot. These packaging checks did not install another driver, perform a new cold boot, or certify another machine.
