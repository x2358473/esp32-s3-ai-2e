# ESP32 ES7210+ES8311+NS4150B Local Audio Demo

([中文](README_zh.md) | English)

This project is a simplified local-audio firmware under `esp32-es7210-es8311/`, focused on ES7210 + ES8311 + NS4150B audio module bring-up and local interaction demo, without cloud/network voice pipeline.

## Core Features

1. **ES7210 + ES8311 + NS4150B driver bring-up (ESP32-S3 / ESP32-P4)**  
   Initializes codec/I2C/audio service on boot and directly plays embedded local PCM audio (`canon.pcm`).

2. **Device-side AEC local barge-in demo (tunable)**  
   When `CONFIG_USE_DEVICE_AEC` is enabled, playback loop supports real-time user speech interruption (barge-in) using local VAD timing thresholds in `main/example_es7210_es8311.c`.

3. **Boot button recording test mode**  
   Press Boot to enter recording test mode during playback, capture microphone audio, press Boot again to stop and play back recorded audio.  
   Recording is in-memory only and **not persisted**.

## Project Layout (Current)

```text
esp32-es7210-es8311/
├─ main/
│  ├─ audio_application.h/.c         # Application state and run entry API
│  ├─ example_es7210_es8311.c        # app_main + board init + button logic + PCM autoplay + barge-in
│  ├─ config.h                       # board pins / sample-rate / packet config
│  ├─ Kconfig.projbuild              # AEC/processor/debug menuconfig options
│  └─ audio/
│     ├─ audio_service.h/.c          # local audio service pipeline (PCM passthrough queues)
│     ├─ audio_codec.h/.c            # codec base
│     ├─ audio_processor.h           # audio processor interface
│     ├─ processors/                 # AFE / no-op processors
│     └─ codecs/                     # box/es8311/es7210 related codec drivers
├─ partitions/partitions.csv         # simplified single-app partition table (4MB-compatible)
├─ sdkconfig.defaults                # common defaults
├─ sdkconfig.defaults.esp32s3        # S3-specific defaults
└─ sdkconfig.defaults.esp32p4        # P4-specific defaults
```

## Build Requirements

- ESP-IDF `v5.4+` (project currently validated with `v5.5`)
- Target: `esp32s3` or `esp32p4`
- For `CONFIG_USE_AUDIO_PROCESSOR` (default `y`): PSRAM required by Kconfig dependency

## Build & Flash

```bash
cd esp32-es7210-es8311
idf.py set-target esp32s3   # or esp32p4
idf.py fullclean
idf.py build
idf.py flash monitor
```

## Runtime Behavior

- On boot, `app_main` is in `main/example_es7210_es8311.c`, which runs:
  - board I2C/button init
  - codec singleton setup
  - audio service start
  - optional autoplay of embedded `main/assets/canon.pcm`
- Boot key behavior:
  - first press: enter local recording test mode
  - second press: stop recording test and play back in-memory recorded audio

## AEC / Barge-in Tuning

In `main/example_es7210_es8311.c`, these parameters are intended for adjustment:

- `kLocalBargeInVadThresholdUs`
- `kLocalBargeInStartupGuardUs`
- `kLocalBargeInArmSilenceUs`

Current defaults in code (`main/example_es7210_es8311.c`):

- `kLocalBargeInVadThresholdUs = 500 * 1000`
- `kLocalBargeInStartupGuardUs = 1000 * 1000`
- `kLocalBargeInArmSilenceUs = 300 * 1000`

Menuconfig steps to enable AEC:

1. Open menuconfig:
   ```bash
   idf.py menuconfig
   ```
2. Go to: `ESP32 ES7210 + ES8311 AEC`
3. Enable `Enable Audio Noise Reduction`
4. Press Enter on it and enable child option `Enable Device-Side AEC`
5. Save and rebuild:
   ```bash
   idf.py build flash monitor
   ```

## FAQ

### 1) Why is `canon.pcm` not played on boot?

- Confirm file exists at `main/assets/canon.pcm`
- Build output should show `Embedded test PCM: ...`; if it shows `canon.pcm not found`, the file is not embedded
- Rebuild with `idf.py fullclean && idf.py build` to refresh `EMBED_FILES`
- The demo expects mono `int16_t` PCM data (local 16kHz test pipeline)

### 2) How to tune the three AEC barge-in thresholds?

In `main/example_es7210_es8311.c`:

- `kLocalBargeInVadThresholdUs`: minimum continuous speech duration before interrupt
- `kLocalBargeInStartupGuardUs`: startup guard window after playback begins
- `kLocalBargeInArmSilenceUs`: required silence duration before arming barge-in
- Default values are `280ms`, `1000ms`, and `180ms` respectively (users can tune for their environment)

Practical tuning tips:

- Too many false interrupts: increase `kLocalBargeInVadThresholdUs` and `kLocalBargeInArmSilenceUs`
- Interrupt is too hard to trigger: decrease `kLocalBargeInVadThresholdUs`
- Interrupted immediately after playback starts: increase `kLocalBargeInStartupGuardUs`

### 3) What are Boot single-click / double-click behaviors?

Current code behavior:

- **Single click**
  - In test mode: toggles between “start local recording test” and “stop + playback recorded audio”
  - Out of test mode: toggles local recording test state
- **Double click**
  - Effective only in test mode: exits test mode and stops current local test flow

## Notes

- The project defaults to **4MB Flash** with a simplified single-app partition table (`partitions/partitions.csv`), and is intended to work on **4MB and larger** Flash sizes.
- If you see build/link/partition/flash errors, adjust `sdkconfig.defaults*` and `partitions/partitions.csv` based on your board's actual Flash size and requirements.
- Audio test recording buffer is transient and cleared by runtime flow; no file persistence is implemented.

## Technical Support

- Code modifications: **surfer**
- Store: `surferai.taobao.com`
