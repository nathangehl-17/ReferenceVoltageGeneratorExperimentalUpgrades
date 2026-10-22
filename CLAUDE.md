# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Reference Voltage Generator: a GTK4 desktop GUI (C) talks over a serial port to an Adafruit Feather ESP32-S3 Reverse TFT, which writes voltages to DAC70501 DACs over I2C (12 channels, 0–5 V, 100 µV steps) and can relay raw bytes over SPI to an Arduino Nano. This repo ("ExperimentalUpgrades") also adds a VHDL I2C master intended to replace the microcontroller's I2C role. `README.md` documents user-facing usage, wiring, and flashing for the v1.0 workflow (images in `images/`).

There is no test suite, linter, or top-level build system.

## Layout

- `GUI_v1.0/` — original release: `main.c` (GUI), `esp32_voltage_generator/` and `spi_nano/` sketches, and a vendored Windows build (`windows_app/`, hundreds of MinGW/GTK DLLs and tools; don't read or edit).
- `GUI_v2.0/` — active work: `main.c` (GUI) and `esp32_voltage_generator/esp32_voltage_generator.ino`. `GUI_v2.0/main` is a committed Linux ELF build artifact.
- `VHDL/` — I2C master hardware design plus testbenches (`tb_*.vhd`). Block diagrams/state graphs in `images/` (sources in `images/Inkscape/`).

## Build / run

GUI (Linux, native; run from the version directory):
```
gcc $(pkg-config --cflags gtk4) -o main main.c $(pkg-config --libs gtk4) -lm && ./main
```
Windows cross-compile (via quasi-msys2): `x86_64-w64-mingw32-gcc main.c -o ReferenceVoltageGenerator.exe \`pkg-config --cflags --libs gtk4\` -lm`. `main.c` is a single file with `#ifdef _WIN32` branches for serial I/O (Win32 HANDLE vs termios fd), so keep both paths working when touching serial code.

Firmware: Arduino IDE (Verify / Export Compiled Binary). On Linux the Feather is flashed with `esptool` as described in `README.md`; the Nano uses a normal IDE upload.

VHDL: simulated with ModelSim/Questa (`i2c.mpf`, `work/`, `vsim.wlf`, `wave.do` are gitignored). Compile in dependency order: primitives (`D_FLIPFLOP`, `R_SHIFT_REG`, `counter_w_EN`, `LimitLogic`, `MUX*`) → `fsm_i2c_writing` → `i2c_writing` → `fsm_i2c_writing_master` → `i2c_writing_master` → `fsm_i2c_master` → `i2c_master`. Each level has a matching `tb_*` testbench (except the top-level `fsm_*`/primitive files).

## Architecture

**GUI ↔ firmware protocol (v2.0).** The GUI builds a binary command in `send_voltage_command()` (`GUI_v2.0/main.c`): `DAC_CMD_START, bank, DAC address, register, value_high, value_low`. Channel → `bank = channel / DACS_PER_BANK`, `chip = channel % DACS_PER_BANK`, with the I2C address looked up in `DAC_ADDR[chip]`. Register constants (`REG_SYNC`, etc.) mirror the DAC70501 register map and are duplicated in the `.ino`. ON/OFF map to DAC power-down register writes; UPDATE writes the DAC data register. v1.0 used a different (text-style) command format, so don't mix v1/v2 GUI and firmware.

**Firmware (`GUI_v2.0/.../esp32_voltage_generator.ino`).** `loop()` is a byte-wise parser keyed on the command byte (same constants as the GUI): `0x00` DAC write (6 bytes, writes `reg` at `bank`/`DAC address` over `Wire`/`Wire1`/bit-bang per-bank pins such as `BANK0_SDA`), `0x01` RESET pulse (2 bytes, `RST_PIN`, level set by `RST_ACTIVE_LEVEL`), `0x02` SPI (`02 count bytes...`, up to `SPI_MAX_BYTES`, one chip-select assertion; each byte shifted back is printed as `RX: XX` for the GUI). The GUI's SPI speed/mode/bit-order dropdowns are not sent; the firmware uses fixed 1 MHz MSBFIRST MODE0. Chip select is `NANO_CS_PIN` (12); pins 3-6 and 9-10 are the I2C banks, 11 is RESET.

**Presets (`GUI_v2.0/main.c`, `presets.ini`).** Named channel-voltage sets stored as GKeyFile sections (`[Name]` / `ch0=1.2`) in `presets.ini` in the working directory; the GUI can load, load-and-send, and save the current values.

**VHDL I2C master hierarchy.** `i2c_master` loads data bytes into five shift registers (bank, DAC, register, V-high, V-low), muxes them (`MUX4_1BIT`/`MUX2_3BIT`) and sequences them with `fsm_i2c_master`; it drives `i2c_writing_master` (per-transaction byte/ACK counting via `counter_w_EN` + `LimitLogic`, FSM `fsm_i2c_writing_master`), which drives `i2c_writing` (bit-level SDA/SCL generation: shift register, SCL/bit counters, FSM `fsm_i2c_writing`). `done_rx` on `i2c_master` is a placeholder for a future RS-232 receiver module.
