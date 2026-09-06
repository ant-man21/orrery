#ifndef PINS_H_
#define PINS_H_

/* Pin map for ESP32-S3-DevKitC-1 (full-size devkit, dual USB-C: native USB
 * on "USB" / UART-JTAG bridge on "UART"). Chosen to avoid the octal
 * flash/PSRAM pins (GPIO26-32, GPIO33-37 on N16R8 boards), the strapping
 * pins (0, 3, 45, 46), the native-USB D+/D- pins (19, 20), and GPIO39-42,
 * which are reserved below for an external J-Link's JTAG lines.
 *
 * STM32F411 (Blackpill) -> ESP32-S3-DevKitC-1:
 *   PA4  MOTOR_LATCH   -> GPIO10
 *   PA5  SPI1_SCK      -> GPIO12 (MOTOR_SCK)
 *   PA7  SPI1_MOSI     -> GPIO11 (MOTOR_MOSI)
 *   PB4  BTN_SIM       -> GPIO4
 *   PB5  BTN_RT        -> GPIO5
 *   PB6  BTN_RESET     -> GPIO6
 *   PC13 status LED    -> GPIO2 (external LED; see README for the
 *                          alternative of driving the onboard WS2812 on
 *                          GPIO48 instead)
 *   PB10/14/15 SPI2 (unused slave bus, reserved for a future host link)
 *                      -> GPIO15/16/17, CS on GPIO18
 */

/* Shift-register chain driving the 8 stepper drivers (SPI2_HOST / HSPI). */
#define MOTOR_SPI_HOST   SPI2_HOST
#define MOTOR_SCK_GPIO   12
#define MOTOR_MOSI_GPIO  11
#define MOTOR_LATCH_GPIO 10

/* Front-panel buttons; wired active-low with internal pull-ups, same as
 * the STM32 build (button shorts the pin to GND when pressed). */
#define BTN_SIM_GPIO     4
#define BTN_RT_GPIO      5
#define BTN_RESET_GPIO   6

/* Heartbeat LED (defaultTask). */
#define STATUS_LED_GPIO  2

/* Reserved for a future host-communication bus (CommTask). Not
 * initialized yet -- kept as a placeholder pin group so a slave/peripheral
 * link can be added later without renumbering everything else. */
#define COMM_SPI_SCK_GPIO  15
#define COMM_SPI_MISO_GPIO 16
#define COMM_SPI_MOSI_GPIO 17
#define COMM_SPI_CS_GPIO   18

/* GPIO39-42 are intentionally left unused by this firmware: on the S3
 * DevKitC-1 they double as MTCK/MTDO/MTDI/MTMS, the pins an external
 * J-Link needs for JTAG. See README.md for wiring + OpenOCD setup. */

#endif /* PINS_H_ */
