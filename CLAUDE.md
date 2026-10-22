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

**Firmware (`GUI_v2.0/.../esp32_voltage_generator.ino`).** `loop()` waits for a fixed-length message from `Serial` (115200), calls `dacWriteReg(bank, chip, REG_DAC, code)` over `Wire` (400 kHz, per-bank SDA/SCL pins such as `BANK0_SDA`), updates the TFT, and has an SPI path (`SPI.transfer`, CS on `NANO_CS_PIN`) for the Nano byte-relay feature. Note: the GUI currently sends a 6-byte frame (leading start byte) while the firmware's `MSG_LEN` is 5 and indexes `msg_buf[0]` as the bank — the v2.0 framing is mid-change; check both sides when editing the protocol. Also, the firmware currently ignores `reg` when writing (always `REG_DAC`).

**VHDL I2C master hierarchy.** `i2c_master` loads data bytes into five shift registers (bank, DAC, register, V-high, V-low), muxes them (`MUX4_1BIT`/`MUX2_3BIT`) and sequences them with `fsm_i2c_master`; it drives `i2c_writing_master` (per-transaction byte/ACK counting via `counter_w_EN` + `LimitLogic`, FSM `fsm_i2c_writing_master`), which drives `i2c_writing` (bit-level SDA/SCL generation: shift register, SCL/bit counters, FSM `fsm_i2c_writing`). `done_rx` on `i2c_master` is a placeholder for a future RS-232 receiver module.
