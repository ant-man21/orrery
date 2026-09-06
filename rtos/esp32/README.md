# Orrery Firmware — ESP32 Port

Port of the STM32F411 (Blackpill) FreeRTOS firmware in `rtos/Orrery` to an
ESP32 target, so the project can grow a network link (for real ephemeris
data) and be debugged over JTAG with an external SEGGER J-Link.

## Decisions made for this port (please confirm)

Two things had to be picked without a follow-up, since this was built
while you were offline. Both are easy to change if they're wrong — say
the word and I'll redo the affected parts.

1. **Board: ESP32-S3-DevKitC-1** (N16R8 or N8R8; "full-size" devkit, not
   one of the -Mini/-Zero boards). Reasoning:
   - It's the standard "full size" ESP32 devkit form factor.
   - It ships with USB-C (two of them, actually — see below).
   - The S3 has WiFi + BT for the internet link you mentioned wanting
     later.
   - The S3 exposes dedicated JTAG pins (GPIO39-42) for exactly the
     external J-Link debugging you asked about — the plain ESP32 (no
     "S3"/"C3" suffix) doesn't have a documented external-JTAG story as
     clean as the S3's, and has no native USB.
   - If you actually meant the plain ESP32 (no native USB, JTAG only via
     bit-banged/limited support) or an S2/C3 variant, let me know and
     I'll re-map pins accordingly — the application logic in `stepper.c`
     doesn't change either way.
   - DevKitC-1 has **two USB-C ports**: one labeled `USB` (native USB-OTG,
     wired straight to the S3's own USB peripheral) and one labeled `UART`
     (USB-to-serial bridge for flashing/console on boards that have it).
     Either works for flashing; the README below assumes whichever one
     enumerates as a serial port for you.

2. **Framework: ESP-IDF, not Arduino.** You asked directly, so here's the
   reasoning:
   - The STM32 build already uses FreeRTOS tasks directly (CMSIS-RTOS2 on
     top of raw HAL) — ESP-IDF's FreeRTOS is the same relationship,
     mapping almost 1:1 (`osThreadNew` → `xTaskCreatePinnedToCore`, etc.).
     Arduino-ESP32 hides the RTOS underneath `setup()`/`loop()`, which
     fights against "I want it to stay RTOS."
   - J-Link/OpenOCD/GDB debugging is a first-class, well-documented
     ESP-IDF workflow (`idf.py openocd`, `idf.py gdb`). It works from
     Arduino too (it's the same chip), but the tooling and docs are built
     around ESP-IDF projects.
   - You still get all the same libraries later: Arduino-ESP32 can be
     pulled in as *just another ESP-IDF component* if you want a specific
     Arduino library for the internet work, without giving up the native
     FreeRTOS structure. Best of both, if it comes to that.

## What changed vs. the STM32 build

The physical/orbital-mechanics logic in `stepper.c` is untouched — same
gear ratios, orbital periods, half-step table, SIM/REALTIME/homing state
machine. Only the hardware layer changed:

| STM32 (Blackpill)                         | ESP32-S3 port                              |
|--------------------------------------------|---------------------------------------------|
| HAL_SPI (SPI1, master, TX-only)           | ESP-IDF `spi_master` driver (`SPI2_HOST`)   |
| `HAL_GPIO_*`                               | `driver/gpio.h`                             |
| DWT cycle-counter `get_micros()`           | `esp_timer_get_time()` (native µs timer)    |
| IWDG (`HAL_IWDG_Init` + refresh)           | Task Watchdog Timer (`esp_task_wdt_*`)      |
| CMSIS-RTOS2 (`osThreadNew`)                | native FreeRTOS (`xTaskCreatePinnedToCore`) |
| SPI2 (slave, initialized but never used)   | pins reserved in `pins.h`, not wired up yet |

`get_micros()` got simpler, not just different: the STM32 code has a long
comment explaining a rollover workaround needed because `DWT->CYCCNT` is
only effectively 28 bits wide at 16MHz. `esp_timer_get_time()` is a real
free-running 64-bit microsecond counter, so that workaround is gone —
`get_micros()` is now a one-line truncation to 32 bits, which is all
`Motor_Poll()`'s wrap-safe timing math actually needed.

## Pin map

See `main/pins.h` for the full table and reasoning (which pins are
avoided and why: octal flash/PSRAM, strapping pins, native-USB D+/D-, and
GPIO39-42 reserved for JTAG). Short version:

| Signal         | ESP32-S3 GPIO |
|----------------|---------------|
| MOTOR_SCK      | 12            |
| MOTOR_MOSI     | 11            |
| MOTOR_LATCH    | 10            |
| BTN_SIM        | 4             |
| BTN_RT         | 5             |
| BTN_RESET      | 6             |
| STATUS_LED     | 2 (external LED; onboard WS2812 is GPIO48 but needs a driver, not a plain GPIO toggle) |

Wire the shift-register chain and buttons to the new GPIOs above instead
of the old STM32 pins; everything else (8x 28BYJ-48 via ULN2003 through a
595-style shift chain, 3 momentary buttons to GND) is unchanged.

## Building — reproducibly, with minimal setup

This has been built and verified end-to-end against real ESP-IDF **v5.3.1**
(the exact version pinned below) — a clean `idf.py build` compiles
`main.c` and `stepper.c` with zero warnings and links a
`orrery_esp32.bin` (~233KB image, well under the default 1MB app
partition). That's not just "should work" — it was actually compiled.

**Easiest path: Docker, one command, nothing to install.**

```sh
cd rtos/esp32
./build.sh
```

That's it. `build.sh` runs `idf.py set-target esp32s3 && idf.py build`
inside Espressif's official `espressif/idf:v5.3.1` container, mounting
this directory in. No ESP-IDF install, no Python environment, no PATH
setup — just Docker. It's pinned to that exact image tag (not `latest`),
so it builds against the same compiler and SDK version every time, on
your machine or anyone else's. Pass any `idf.py` subcommand as an
argument, e.g. `./build.sh fullclean`.

**Also reproducible: CI builds it on every push.** `.github/workflows/build.yml`
has an `esp32` job that runs this exact same build (same pinned image tag)
on every push and PR, and uploads the resulting `.bin`/`.elf` as a build
artifact — so a red CI check means "this doesn't compile," not "no one
checked."

**For flashing and J-Link debugging** you need direct USB/JTAG device
access, which the Docker container doesn't have by default, so install
ESP-IDF natively for that part:

```sh
git clone -b v5.3.1 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3 && source ./export.sh

cd rtos/esp32
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor   # port varies; use whichever USB-C port enumerates
```

Pin the same `v5.3.1` tag as `build.sh`/CI so all three build the same
bits. `sdkconfig.defaults` already pins the target to `esp32s3` and sets
the watchdog timeout to 4s (matching the STM32 IWDG), so `set-target`
only needs to run once per fresh `build/` directory.

<details>
<summary>Troubleshooting: pip install hangs or fails during <code>install.sh</code></summary>

If your network blocks `dl.espressif.com` (some corporate/sandboxed
networks do — that's what happened while building this), `install.sh`
either hangs retrying a Python package mirror or fails outright fetching
`espidf.constraints.v5.3.txt`. Two fixes, both needed:

```sh
export IDF_PYTHON_CHECK_CONSTRAINTS=no   # skip the blocked constraints file
./install.sh esp32s3
source ./export.sh
```

Skipping constraints means pip resolves `idf-component-manager` to the
latest PyPI release instead of the version IDF 5.3.1 was tested against —
the newest one (3.x) speaks a component-manager protocol IDF 5.3.1
doesn't understand and `idf.py build` fails with
`argument --interface_version: invalid choice`. Fix by pinning it back
to a contemporary release:

```sh
pip install "idf-component-manager==1.5.3"
```

None of this affects `build.sh` or CI — the official Docker image already
bundles a matched, working `idf-component-manager`, so this only matters
for a native install on a similarly restricted network.
</details>

## Debugging / single-stepping with a J-Link

The S3's JTAG pins are fixed and already reserved for this in `pins.h`:

| JTAG signal | GPIO |
|-------------|------|
| TMS         | 42   |
| TCK         | 39   |
| TDI         | 41   |
| TDO         | 40   |

Wire your J-Link's TMS/TCK/TDI/TDO to those four pins, plus GND and
VTref (3.3V) from the board. Then, with the board also connected over
USB for flashing/console:

```sh
# Terminal 1: OpenOCD, using ESP-IDF's bundled config + SEGGER's J-Link driver
openocd -f interface/jlink.cfg -f target/esp32s3.cfg

# Terminal 2: build + flash as usual, then launch GDB against OpenOCD
idf.py build flash
idf.py gdb
```

`idf.py gdb` drops you at a GDB prompt already attached to the running
target — `break MotionTaskStart`, `continue`, `next`/`step`, etc. all
work as normal. If `openocd` isn't on your PATH, ESP-IDF ships its own
copy under `$IDF_PATH/../.espressif/tools/openocd-esp32/`; `idf.py
openocd` runs that copy for you instead of the two-terminal dance above.

Alternative: the S3 also has a built-in USB-JTAG bridge on its native USB
port, so `idf.py openocd`/`idf.py gdb` will work over a single USB-C
cable with no J-Link at all if you ever want to debug without external
hardware. Since you specifically want to use the J-Link, the external
wiring above is what to use; the pins are just reserved either way.

## Future internet work

`CommTask` in `main.c` is an intentional no-op stub, same as the STM32
build — it's the hook point for the network client that will eventually
feed `Motor_SetModeRealtime()` real ephemeris data instead of the current
"90 degrees per button press" placeholder. Nothing else in the motion
logic needs to change to support that; `MODE_REALTIME` already exists and
just needs a real data source driving it.
