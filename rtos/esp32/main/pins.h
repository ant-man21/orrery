#ifndef PINS_H_
#define PINS_H_

/* Pin map for a classic ESP32-WROOM-32 dev board (USB-C variant; single
 * USB-C port -- power + a USB-serial bridge chip (CP2102/CH340), no
 * native USB peripheral on this chip at all, unlike the S3). Chosen to
 * avoid:
 *   - GPIO6-11:  wired internally to the module's SPI flash -- never use
 *   - GPIO0/2/15: boot-strapping pins (2 is reused below for the status
 *                 LED anyway, which is exactly what most WROOM-32 devkits
 *                 already wire it to -- safe as an output post-boot)
 *   - GPIO1/3:    UART0, used for flashing/the serial console
 *   - GPIO34-39:  input-only, no internal pull-up/down -- unusable for
 *                 the push-button inputs below, which rely on one
 *   - GPIO12-15:  reserved for external J-Link JTAG (see README) --
 *                 GPIO12 (MTDI) is ALSO a strapping pin that can send the
 *                 board into a bad boot (wrong flash voltage) if it's
 *                 pulled high at reset, so leave this whole group alone
 *
 * STM32F411 (Blackpill) -> ESP32-WROOM-32:
 *   PA4  MOTOR_LATCH   -> GPIO19
 *   PA5  SPI1_SCK      -> GPIO18 (MOTOR_SCK)
 *   PA7  SPI1_MOSI     -> GPIO23 (MOTOR_MOSI)
 *   PB4  BTN_SIM       -> GPIO32
 *   PB5  BTN_RT        -> GPIO33
 *   PB6  BTN_RESET     -> GPIO25
 *   PC13 status LED    -> GPIO2 (many WROOM-32 devkits already have an
 *                          onboard LED wired here -- check yours before
 *                          adding an external one)
 *   PB10/14/15 SPI2 (unused slave bus, reserved for a future host link)
 *                      -> GPIO27/26/21, CS on GPIO22
 */

/* Shift-register chain driving the 8 stepper drivers (SPI2_HOST / HSPI).
 * Custom (non-IOMUX) pins go through the GPIO matrix instead of the
 * chip's fast dedicated SPI routing -- irrelevant at the ~1MHz these
 * shift registers run at. */
#define MOTOR_SPI_HOST   SPI2_HOST
#define MOTOR_SCK_GPIO   18
#define MOTOR_MOSI_GPIO  23
#define MOTOR_LATCH_GPIO 19

/* Front-panel buttons; wired active-low with internal pull-ups, same as
 * the STM32 build (button shorts the pin to GND when pressed). Must be
 * pins that support pull-ups -- GPIO34-39 don't, so none of these three
 * are in that range. */
#define BTN_SIM_GPIO     32
#define BTN_RT_GPIO      33
#define BTN_RESET_GPIO   25

/* Heartbeat LED (defaultTask). */
#define STATUS_LED_GPIO  2

/* Reserved for a future host-communication bus (CommTask). Not
 * initialized yet -- kept as a placeholder pin group so a slave/peripheral
 * link can be added later without renumbering everything else. */
#define COMM_SPI_SCK_GPIO  27
#define COMM_SPI_MISO_GPIO 26
#define COMM_SPI_MOSI_GPIO 21
#define COMM_SPI_CS_GPIO   22

/* GPIO12-15 are intentionally left unused by this firmware: they're the
 * chip's fixed JTAG pins (MTDI/MTCK/MTMS/MTDO) for an external J-Link.
 * See README.md for wiring + OpenOCD setup, and the GPIO12 boot-voltage
 * warning above. */

#endif /* PINS_H_ */
