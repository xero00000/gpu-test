# Qualified NVIDIA unlock workflow

This guide applies to explicit unlock requests. Benchmarking remains the default GPU testing workflow. The shared entry point is `python3 scripts/gpu_tool.py`.

## Included implementation

- `unlock/ampere`: source snapshot of [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock/tree/1799023e1d16dd1e58a7fc85823579fe93281e1a), with its source, host tests, pinned asset recipes, and licenses. Its module was previously tested here on RTX 3080 LHR `10de:2216` / subsystem `19da:1612` and RTX 3060 Ti LHR `10de:2489` / subsystem `196e:1389`. The shared CLI limits selection to those qualified device/subsystem combinations.
- `unlock/tu104`: the board-specific EVGA RTX 2080 SUPER port used in the earlier hardware trial. It preserves the exact bus `0000:05:00.0`, PCI `10de:1e81`, subsystem `3842:3081`, TU104 chip/8 GiB checks, and byte-for-byte ROM comparison. It uses Turing FWSEC/Booter loading and cleanup rather than the Ampere path. The CLI additionally checks the supplied ROM hash before building.
- `unlock/rm_issue_rate.c`: a read-only diagnostic from the pinned Turing reference. It uses NVIDIA RM GET controls to report issue-rate modifiers; it is not a benchmark or an unlock command.
- `unlock/provenance.json`: upstream commits, source hashes, asset hashes, and the qualified environment. The module source hashes are checked before building. No module device-write sequence was changed when packaging these sources.

Only kernel **7.2.10-200.nobara.fc44.x86_64**, driver **595.104.02**, Linux x86-64, Secure Boot disabled, and **dracut with a plain BLS entry** are accepted by the installer. This is the configuration used in the prior trials. Other environments need separate qualification; a successful compile alone is insufficient. Windows/AMD/Intel/Apple unlocks are not implemented.

The TU104 ROM hash is `29d78dea1c11a6094f0297bdc8ed5afcfb959166fb51f884ade2c2e679d84878`. A different ROM, another board, or another address is refused. This tool does not extract a ROM or flash one. Do not relax the checks to make an unsupported GPU pass.

## Prepare without changing the system

1. Run `inventory` and inspect the actual GPU identities. Preserve existing workloads and any existing unlock installation.
2. Save a short `bench` baseline, using the exact runtime/device/size options you will repeat afterward. For model speed, use the actual model profile and its separate inference runner.
3. Use `unlock-plan --targets <explicit-comma-separated-PCI-addresses> --output <new-plan.json>`. The plan records the kernel, driver, and PCI/board identities. There is no wildcard or `all` unlock selection.
4. Run `unlock-build --plan <plan.json> --output <new-kit-dir>`. Supply `--fetch` to allow downloading missing pinned inputs, or `--inputs <cache>` to use an existing cache. TU104 also needs `--board-rom <captured-ROM>`. `--turing-payload <local-file>` can supply the pinned payload offline.

The build requires Python 3.11+, make, a C compiler, matching kernel headers, `modinfo`, and support for reading the zstd-compressed input package (`zstd` or the corresponding Python support). Missing inputs fail without an automatic package/driver installation. Input downloads total about 326 MB; the Turing reference payload adds 64 KB. Inputs are data, not installed driver packages.

The build validates input SHA-256, extracts exact source members, generates checked firmware, runs the Ampere host suites and (when selected) the Turing full transaction mock, compiles kernel modules, checks their vermagic, and builds the read-only issue-rate diagnostic. `kit.json` is written only after these stages pass. The kit contains asset/module hashes and a qualification statement. Existing output directories are refused, preserving failure receipts.

Firmware is generated in the private kit directory. It is not committed to Git or distributed as a release binary. An owner-supplied ROM is also kept local. The Turing payload is obtained from its pinned reference URL and checked against its exact hash when no local copy is supplied. The NVIDIA package license is retained alongside generated firmware. See [third-party notices](../THIRD_PARTY.md).

## Install a separate trial

Run the explicit `unlock-install` subcommand with `--dry-run`, the kit directory, and the absolute path to an existing **normal** BLS entry. Root may be needed just to read `/boot`. Inspect the resulting entry, targets, and paths. An actual install uses the same command without `--dry-run` and requires root.

The installer:

- Checks the current kernel/driver, PCI identities, kit hashes, Secure Boot state, required tools, and plain BLS layout. It refuses existing unlock modules instead of installing another copy that might shadow them.
- Copies modules to a uniquely named `extra/gpu-test-<name>` directory and checked firmware to its declared firmware paths. Identical pre-existing firmware is preserved; differing files are refused.
- Builds an independent initramfs, verifies embedded module/firmware bytes and pre-driver ordering, and publishes the new BLS entry last. It checks that the normal entry's contents did not change.
- Saves an installation journal and the normal entry under `/var/lib/gpu-test/gpu-test-<name>/`. A failure removes unchanged files created by that attempt and preserves the failure journal.

No GRUB default or one-shot selection is changed. The tool does not flash, unbind a live GPU, hot-load an unlock module, power-cycle hardware, or reboot. It does not create DKMS or future-kernel upgrade hooks. Firmware modification is applied to volatile GPU state during the selected cold boot.

Save work, shut down completely, power on, and manually select **GPU Test - tensor unlock TRIAL (cold boot only)**. A warm reboot is not a supported substitute. If the trial fails, power off fully and choose the normal boot entry; do not make repeated warm attempts.

## Verify and measure

`unlock-verify --state /var/lib/gpu-test/gpu-test-<name>/state.json --output <new-report.json>` reads this boot's kernel logs, target selection, NVIDIA device-minor mapping, and RM issue-rate values. It requires successful handoff logs, no matching GPU errors, and full FMLA32/IMLA4 modifiers for all selected targets. The installed read-only helper's hash is checked before it runs.

Then run the same short benchmark used before the trial. Issue-rate values establish what the driver reports, not measured throughput. Compare numerical checks and errors as well as speed. Keep reports separate from the original historical cuBLAS case study.

The modules and assets were previously exercised on the three hardware cards in the README. The new package has build, host/mock, and installer dry-run validation. **No new cold-boot trial of this packaged installer has been claimed.**

## Remove a trial

`sudo python3 scripts/gpu_tool.py unlock-remove --state /var/lib/gpu-test/gpu-test-<name>/state.json` checks the state location, owned path names, entry/image hashes, and GRUB's saved/next selections. A selected or externally modified entry is preserved for manual review. The command removes only its unchanged trial menu entry and image, retaining module/firmware files to avoid breaking another boot entry.

The normal default remains available. Shut down fully and boot normally to restore the stock volatile state. A kernel update requires a new qualified build/image; the tool does not claim that its current kit works on future kernels.
