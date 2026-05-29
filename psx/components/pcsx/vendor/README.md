# PCSX ReARMed Vendor Drop-In

Drop the upstream `PCSX ReARMed` source tree here using this exact folder name:

- `psx/components/pcsx/vendor/pcsx_rearmed/`

Expected example paths after dropping it in:

- `psx/components/pcsx/vendor/pcsx_rearmed/README.md`
- `psx/components/pcsx/vendor/pcsx_rearmed/frontend/main.c`
- `psx/components/pcsx/vendor/pcsx_rearmed/libpcsxcore/`

The current firmware-side port is prepared to switch from stub mode to vendor-present mode when this tree appears.

Recommended upstream:

- `https://github.com/notaz/pcsx_rearmed`

Notes for this target:

- start with software rendering only
- avoid enhancement paths
- prefer simple disc formats like `CHD` or `BIN/CUE`
- target `Final Fantasy Tactics` as the first boot candidate
