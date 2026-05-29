# PSX Port Notes

Current target: `Final Fantasy Tactics`

## Chosen direction

The most realistic PlayStation core to try on ESP32-S3 is `PCSX ReARMed`.

Why:

- it is still the most performance-oriented open-source PS1 core for constrained devices
- it already has a history of lighter software rendering paths
- it is a better fit than DuckStation-class desktop emulators for this hardware

## Hardware reality

Katarina's ESP32-S3 can likely support:

- file browser and disc image handling
- BIOS loading
- basic emulator boot bring-up
- maybe some lightweight 2D scenes if the core is reduced aggressively

Main risks:

- no Xtensa dynarec path
- full software GPU cost
- audio cost under load
- cache/PSRAM pressure

## Best media formats

Preferred order for testing:

1. `CHD`
2. `BIN/CUE`
3. `PBP`

For `Final Fantasy Tactics`, start with a clean single-disc USA dump in `CHD` or `BIN/CUE`.

## BIOS

Expected BIOS names:

- `scph5501.bin`
- `scph1001.bin`

Place BIOS files in:

- `/sd/retro-go/bios/`

## First implementation goals

1. Integrate a dedicated `psx` app partition
2. Add launcher support for PS1 media types
3. Bring up BIOS + disc loading
4. Prepare memory card paths and frontend/core adapter boundary
5. Stub video/audio/input bridge
6. Attempt first boot of FFT title screen

## Current repo status

Already done in this tree:

- dedicated `psx` app partition
- launcher registration for `chd cue pbp img iso bin m3u`
- BIOS probing
- disc-type probing
- memory card path preparation
- dedicated `psx_core` adapter boundary for future core import

## Success criteria for phase 1

- the launcher detects PS1 games
- the `psx` app boots
- BIOS is found
- disc image opens
- the emulator core reaches an identifiable game boot stage
