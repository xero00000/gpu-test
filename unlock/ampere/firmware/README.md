# Pinned 610.43.03 firmware

`make inputs` runs `fetch_artifacts.py`, which downloads the NVIDIA open kernel
module source archive and the exact CachyOS `nvidia-utils` package pinned in
`artifacts-610.43.03.json`. Downloads use HTTPS and are checked against their
SHA-256 before they enter the cache; cached files are rechecked on every run.
Nothing downloaded is executed or installed.

`make firmware` then builds the five files the kernel module loads from
`gpu-unlock/610.43.03/`:

- `generate_v67.py` extracts `gsp_ga10x.bin` and the NVIDIA license from the
  package and generates the 64,000-byte `v67-ga102-610.43.03.bin`.
- `extract_boot_assets.py` builds `booter_load_ga102.bin`,
  `gsp_boot_ga102.bin` and `gsp_boot_desc_ga102.bin` from the source archive.

Both scripts check every input and output against its pinned SHA-256 before
writing anything. Python 3.14+ is required for zstd tar support. See
[NOTICE.md](NOTICE.md) for provenance and licensing.
