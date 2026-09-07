# Orrery Firmware — ESP32 Port

Port of the STM32F411 (Blackpill) FreeRTOS firmware in `rtos/Orrery` to an
ESP32 target, so the project can grow a network link (for real ephemeris
data) and be debugged over JTAG with an external SEGGER J-Link.

## Decisions made for this port

1. **Board: classic ESP32-WROOM-32** (a USB-C devkit, confirmed against
   the actual hardware). This is a meaningfully different chip from the
   ESP32-**S3** this port originally targeted, so it's worth being
   explicit about what that changes:
   - **No native USB.** The WROOM-32 module has no USB peripheral at
     all — the board's single USB-C port is power plus a USB-to-serial
     bridge chip (CP2102 or CH340, depending on the board), same role
     as the ST-Link/USB combo on the STM32 Blackpill.
   - **JTAG is external-only** (GPIO12-15, fixed in silicon) — which is
     exactly what you asked for with the J-Link anyway, so this isn't a
     downside here.
   - **Different danger pins**: GPIO6-11 are wired to the module's own
     flash (never touch them), GPIO0/2/15 are boot-strapping pins,
     GPIO34-39 are input-only (no pull-up/down, so unusable for the
     buttons below), and GPIO1/3 are the UART used for flashing/console.
     `main/pins.h` avoids all of these; see its header comment for the
     full reasoning.
   - WiFi + BT are still onboard, so the internet-link plan is unaffected.

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

| STM32 (Blackpill)                         | ESP32 port                                 |
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
avoided and why: flash pins, boot-strapping pins, input-only pins, UART0,
and GPIO12-15 reserved for JTAG). Short version:

| Signal         | ESP32-WROOM-32 GPIO |
|----------------|---------------------|
| MOTOR_SCK      | 18                  |
| MOTOR_MOSI     | 23                  |
| MOTOR_LATCH    | 19                  |
| BTN_SIM        | 32                  |
| BTN_RT         | 33                  |
| BTN_RESET      | 25                  |
| STATUS_LED     | 2 (many WROOM-32 devkits already have an onboard LED wired here — check yours before adding an external one) |

Both boards run 3.3V logic, so this is a straight pin swap — no level
shifters, no resistor changes. Move the shift-register clock/data/latch
and the three button signal wires from the old STM32 pins to the GPIOs
above; everything downstream (8x 28BYJ-48 via ULN2003 through a
595-style shift chain, 3 momentary buttons' other leg to GND) is
unchanged.

## Building — reproducibly, with minimal setup

This has been built and verified end-to-end against real ESP-IDF **v5.3.1**
(the exact version pinned below) — a clean `idf.py build` compiles
`main.c` and `stepper.c` with zero warnings and links a
`orrery_esp32.bin` (~208KB image, well under the default 1MB app
partition). That's not just "should work" — it was actually compiled,
for both the ESP32-S3 this port was originally scoped for and the
classic ESP32-WROOM-32 the hardware turned out to be.

**Easiest path: Docker, one command, nothing to install.**

```sh
cd rtos/esp32
./build.sh
```

That's it. `build.sh` runs `idf.py set-target esp32 && idf.py build`
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

The Docker container can't reach your USB port, so flashing needs a
different tool — see **Flashing** below, which doesn't need the full
toolchain either.

## Flashing — like clicking "Run" in STM32CubeIDE

Building and flashing are separate steps here, and flashing is the
lightweight one: `esptool` (a small pip package, no compiler toolchain)
is enough to write already-built `.bin` files to the board. You never
need the full ESP-IDF install just to flash.

```sh
cd rtos/esp32
./flash.sh              # auto-detects the port, flashes ./build
```

That's the one-command equivalent of the STM32CubeIDE Run button. It
installs `esptool` on first use if it's missing, auto-detects the USB
serial port, and writes the bootloader + partition table + app in one
shot using the exact offsets `idf.py build` generated
(`build/flash_args`). Pass a port explicitly if auto-detect picks the
wrong one (e.g. two boards plugged in): `./flash.sh /dev/ttyACM0`.

**Don't have a local build?** Every CI run now uploads a `firmware-esp32`
artifact with everything `flash.sh` needs — download it from the PR's
"Checks" tab or the Actions run, unzip it, and run:

```sh
BUILD_DIR=~/Downloads/firmware-esp32 ./flash.sh
```

**Entering flash mode:** the board's USB-serial bridge chip auto-resets
it into the bootloader for you, same as an ST-Link does for the STM32
build — no button needed in the normal case. If a flash attempt times
out waiting for the chip to respond, put it in bootloader mode by hand:
hold **BOOT**, tap **RESET** (sometimes labeled **EN**), release
**BOOT**, then re-run `./flash.sh`.

To watch serial output afterward: `pip install --user esp-idf-monitor`
then `python3 -m esp_idf_monitor --port /dev/ttyACM0` (or `idf.py
monitor` if you have the full toolchain installed for J-Link work below).

<details>
<summary>Troubleshooting: pip install hangs or fails during <code>install.sh</code></summary>

If your network blocks `dl.espressif.com` (some corporate/sandboxed
networks do — that's what happened while building this), `install.sh`
either hangs retrying a Python package mirror or fails outright fetching
`espidf.constraints.v5.3.txt`. Two fixes, both needed:

```sh
export IDF_PYTHON_CHECK_CONSTRAINTS=no   # skip the blocked constraints file
./install.sh esp32
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

None of this affects `build.sh`, `flash.sh`, or CI — the official Docker
image already bundles a matched, working `idf-component-manager`, so
this only matters for a native ESP-IDF install (needed for J-Link
debugging below) on a similarly restricted network.
</details>

## Debugging / single-stepping with a J-Link

This needs the full native ESP-IDF install (OpenOCD + GDB aren't part of
`flash.sh`'s lightweight esptool-only path):

```sh
git clone -b v5.3.1 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32 && source ./export.sh
```

The chip's JTAG pins are fixed in silicon and already reserved for this
in `pins.h`:

| JTAG signal | GPIO | Also known as |
|-------------|------|---------------|
| TMS         | 14   | MTMS          |
| TCK         | 13   | MTCK          |
| TDI         | 12   | MTDI          |
| TDO         | 15   | MTDO          |

**Watch GPIO12 (TDI):** it doubles as a boot-strapping pin that selects
flash voltage. If it's pulled high while the board powers on or resets,
most WROOM-32 modules (3.3V flash) will fail to boot. This is normally
only a risk while your J-Link is actively driving the JTAG lines during
a debug session, not during ordinary flashing/running — but if the
board won't boot with the J-Link connected, disconnect the J-Link's TDI
line (or the whole probe) before power-cycling, then reconnect once
it's up.

Wire your J-Link's TMS/TCK/TDI/TDO to those four pins, plus GND and
VTref (3.3V) from the board. Then, with the board also connected over
USB for flashing/console:

```sh
# Terminal 1: OpenOCD, using ESP-IDF's bundled config + SEGGER's J-Link driver
openocd -f interface/jlink.cfg -f target/esp32.cfg

# Terminal 2: build + flash as usual, then launch GDB against OpenOCD
idf.py build flash
idf.py gdb
```

`idf.py gdb` drops you at a GDB prompt already attached to the running
target — `break MotionTaskStart`, `continue`, `next`/`step`, etc. all
work as normal. If `openocd` isn't on your PATH, ESP-IDF ships its own
copy under `$IDF_PATH/../.espressif/tools/openocd-esp32/`; `idf.py
openocd` runs that copy for you instead of the two-terminal dance above.

Unlike the S3, this chip has no native USB, so there's no single-cable
USB-JTAG alternative — the external J-Link wiring above is the only way
to get JTAG on this board, which is exactly what you asked for anyway.

## Future internet work

`CommTask` in `main.c` is an intentional no-op stub, same as the STM32
build — it's the hook point for the network client that will eventually
feed `Motor_SetModeRealtime()` real ephemeris data instead of the current
"90 degrees per button press" placeholder. Nothing else in the motion
logic needs to change to support that; `MODE_REALTIME` already exists and
just needs a real data source driving it.
