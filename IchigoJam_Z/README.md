# IchigoJam_Z

## What is this?

**IchigoJam_Z** is a port of [IchigoJam BASIC](https://ichigojam.net/) to the [Zephyr RTOS](https://zephyrproject.org/).

IchigoJam is a tiny single-board computer designed for learning programming, featuring its own BASIC interpreter. This port runs the IchigoJam BASIC core on Zephyr, making it available on multiple microcontroller platforms with a unified codebase.

The key design goal is **board portability through Devicetree overlays**: adding support for a new board requires only a new `.overlay` file — no changes to the HAL source files (`.h`) are needed.

### Supported boards

| Board | MCU | Status |
|---|---|---|
| Raspberry Pi Pico | RP2040 | Verified |
| FRDM-MCXA153 | MCXA153 | Verified |
| FRDM-MCXC444 | MCXC444 | Verified |

### Implemented features

| Feature | IchigoJam commands |
|---|---|
| UART I/O | all text I/O |
| GPIO output | `OUT` |
| GPIO input / ADC | `IN()`, `ANA()` |
| PWM / sound | `BEEP`, `PLAY` |
| Flash storage | `SAVE`, `LOAD` |
| I2C | `I2C()` |
| Machine code call | `USR()` |

---

## Build

### Prerequisites

- Zephyr SDK and `west` installed (Zephyr 4.4 or later)
- A workspace with this repository checked out

### Build commands

```zsh
# Raspberry Pi Pico
west build -b rpi_pico

# FRDM-MCXA153
west build -b frdm_mcxa153

# FRDM-MCXC444
west build -b frdm_mcxc444
```

Run from the repository root. The build system picks up `IchigoJam_Z/` as the application directory.

### Flash

**Raspberry Pi Pico (no SWD):**
Hold the BOOTSEL button while connecting USB, then copy `build/zephyr/zephyr.uf2` to the mass-storage drive that appears.

**Raspberry Pi Pico (with SWD):**
```zsh
west flash --openocd /usr/local/bin/openocd
```

**FRDM boards:**
```zsh
west flash
```

---

## Pin assignments

Pin assignment details for each board are in the `pinmap/` directory:

| Board | File |
|---|---|
| Raspberry Pi Pico | [`pinmap/rpi_pico/pico.txt`](pinmap/rpi_pico/pico.txt) |
| FRDM-MCXA153 | [`pinmap/frdm_mcxa153/frdm_mcxa153.txt`](pinmap/frdm_mcxa153/frdm_mcxa153.txt) |
| FRDM-MCXC444 | [`pinmap/frdm_mcxc444/frdm_mcxc444.txt`](pinmap/frdm_mcxc444/frdm_mcxc444.txt) |

The Devicetree overlay for each board (`boards/<board>.overlay`) is the authoritative source of peripheral assignments and is what the firmware actually uses.

### Quick reference — Raspberry Pi Pico

| IchigoJam function | GPIO |
|---|---|
| UART TX / RX | GP0 / GP1 |
| OUT1 – OUT4 | GP8 – GP11 |
| OUT5 / OUT6 | GP22 / GP21 |
| IN1 / ANA(1) | GP27 (ADC ch1) |
| IN2 / ANA(2) | GP26 (ADC ch0) |
| IN3 / IN4 | GP6 / GP7 |
| LED (on-board) | GP25 |
| BTN | GP28 (ADC ch2) |
| SOUND | GP20 (PWM slice2 chA) |
| I2C SDA / SCL | GP4 / GP5 |

---

## Directory structure

```
IchigoJam_Z/
├── CMakeLists.txt
├── prj.conf              # Common Kconfig for all boards
├── boards/
│   ├── rpi_pico.overlay
│   ├── frdm_mcxa153.overlay
│   └── frdm_mcxc444.overlay
├── pinmap/               # Human-readable pin assignment tables
│   ├── rpi_pico/
│   ├── frdm_mcxa153/
│   └── frdm_mcxc444/
└── src/
    ├── main.c
    ├── config.h
    ├── ichigojam-io.h    # GPIO / ADC / I2C / PWM HAL
    ├── sound.h           # BEEP / PLAY
    ├── flashstorage.h    # SAVE / LOAD (NVS)
    ├── display.h
    ├── keyboard.h
    ├── system.h
    └── usr.h             # USR() machine-code call
```