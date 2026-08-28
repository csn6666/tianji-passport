# Repository Guidelines

## Project Structure & Module Organization

This repository is `tianji-passport`: a fortune-telling device built on the ESP32-C3 FoloToy AI Passport board.

- `components/bsp/include/`: public BSP APIs and the hardware pin/configuration source of truth (`bsp_pins.h`).
- `components/bsp/src/`: display, button, audio, battery, and shared-I2C implementations.
- `main/`: the application. `demo_fortune.c` holds every page and interaction; `bazi_engine.c` + the generated `bazi_tables.c` are the on-device chart engine (pure C99, also compiles on the host for cross-checking); `app_net.c` is the Wi-Fi/SNTP singleton and `chat_provision.c` the phone-based provisioning flow.
- `server/`: the WebSocket backend (ASR -> LLM -> TTS) plus `server/bazi/`, the Python chart engine the on-device tables are baked from.
- `tools/`: `gen_bazi_tables.py` bakes the tables, `verify_bazi.sh` cross-checks both engines field by field, `check_secrets.sh` guards against leaking keys before publishing.
- `sdkconfig.defaults`: reproducible target, console, LVGL, and memory defaults.
- `docs/HARDWARE_BASELINE.md`: wiring, known hardware traps, and the on-device acceptance checklist (upstream baseline).

Changing `bazi_engine.c` or the table generator means re-running `./tools/verify_bazi.sh` before flashing — the on-device chart must stay byte-identical to the Python engine.

Keep reusable hardware logic in `components/bsp`; keep board demonstration and UI behavior in `main`.

## Build, Test, and Development Commands

Use ESP-IDF 5.5.x:

```bash
get_idf553                    # Enter the repository's ESP-IDF 5.5.3 environment
idf.py set-target esp32c3     # Configure a fresh checkout
idf.py build                  # Compile firmware and validate dependencies
idf.py flash monitor          # Flash the connected board and open logs
idf.py fullclean              # Remove generated build state when configuration is stale
```

There is no host-side automated test suite currently. Treat a clean `idf.py build` as the minimum check, then run every applicable item in the README acceptance checklist on real hardware.

## Coding Style & Naming Conventions

Write C using four-space indentation and K&R-style braces, following nearby files. Use `snake_case` for functions and locals, `BSP_*` for public hardware constants, and `s_` for file-local state. Keep BSP APIs prefixed with `bsp_`; name demo entry points `demo_<feature>_<action>`. Prefer `static` for internal symbols. UI text stays English; explanatory comments may be Chinese. Preserve comments documenting hardware-specific register values and memory constraints.

## Testing Guidelines

Before submitting, build from the repository root and inspect warnings. On hardware, verify menu navigation and the affected Display, Button, Audio, or Battery page. For pin, display-rotation, codec-clock, ADC, or DMA changes, explicitly record the observed hardware result in the PR. Do not increase LVGL buffers or audio allocations without checking ESP32-C3 internal RAM usage; the board has no PSRAM.

## Commit & Pull Request Guidelines

History follows Conventional Commit-style subjects such as `feat(bsp): ...`, `feat(demo): ...`, `fix(bsp): ...`, and `docs: ...`. Keep commits focused by subsystem. Pull requests should explain the hardware/revision tested, summarize behavior changes, list build and on-device results, and include photos or screenshots for display changes. Link related issues and call out wiring, pin-map, or compatibility impacts.
