# GBA (gbsp) Emulator — Katarina ESP32-S3 Crash Bug

## Symptom

White screen, instant crash:
```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited). Exception was unhandled.
PC: 0x42073cf6  EXCVADDR: 0xc1cd7384
LzmaDec_TryDummy at LzmaDec.c:891
```

## Key observations

1. **CPU at 160MHz, not 240MHz** — sdkconfig has wrong CPU frequency
2. **Flash in DIO mode** (2-bit), not QIO (4-bit) — slower flash access
3. **LZMA code in binary** — PSX CHD decompression library shouldn't be in GBA at all
4. **Checksum mismatch** — flashed binary doesn't match build
5. **`flash io: dio`** and `cpu freq: 160000000 Hz` in boot log

## Root cause

The `rg_tool.py build-fw` uses a shared sdkconfig across all apps. The PSX sdkconfig (with custom PSRAM/cache/CPU settings, custom partition table) contaminated the gbsp build. Building gbsp standalone also fails because:

1. `gbsp/CMakeLists.txt` was missing `RG_PROJECT_APP` and `RG_PROJECT_VER` (fixed — added)
2. The dummy partition table in `rg_tool.py` (line 127: `f.write("dummy, app, ota_0, 65536, 4194304\n")`) is too small for some apps
3. The gbsp sdkconfig inherits wrong settings from previously-built apps

## What needs to be fixed

### 1. CPU frequency (sdkconfig)
gbsp must run at 240MHz, not 160MHz. Add to `gbsp/sdkconfig.defaults` (create if not exists):
```
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=240
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ_240=y
```

### 2. Flash mode
Flash must use QIO mode. Add:
```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```

### 3. PSRAM
Enable PSRAM (gbsp needs it for ROM):
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MODE_OCT=y
```

### 4. Partition table
The dummy partition table used during build must be large enough. `rg_tool.py` line 127 writes a dummy table. Need 2MB+ for gbsp:
```python
# In rg_tool.py build_app():
f.write("dummy, app, ota_0, 65536, 4194304\n")  # 4MB (already done)
```
And gbsp partition in PROJECT_APPS is 2097152 (2MB) — already set.

### 5. Build isolation
gbsp must build with its own clean sdkconfig, not inherit from other apps. Either:
- Create `gbsp/sdkconfig.defaults` with all required settings
- Or build gbsp FIRST before other apps
- Or use `rg_tool.py build gbsp` separately before `build-fw`

### 6. Memory/PSRAM config
The crash at `EXCVADDR: 0xc1cd7384` suggests accessing uninitialized PSRAM. Add:
```
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=32768
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
```

## Files included in zip

- `gbsp/CMakeLists.txt` — app build config
- `gbsp/main/main.c` — app entry point
- `gbsp/main/CMakeLists.txt` — main component
- `gbsp/components/gbsp-libretro/CMakeLists.txt` — emulator core
- `gbsp/sdkconfig` — current (broken) sdkconfig
- `rg_tool.py` — multi-app build script (relevant sections)
- `base.cmake` — shared build config
