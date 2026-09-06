/*
 * Orrery firmware - ESP32-S3 port.
 *
 * Ported from the STM32F411 (Blackpill) build in rtos/Orrery. Same three
 * FreeRTOS tasks, same motor/button behavior in stepper.c; only the
 * hardware glue below (GPIO/SPI setup, task creation, watchdog) changed
 * to target ESP-IDF's native FreeRTOS instead of STM32 HAL + CMSIS-RTOS2.
 *
 * Board: ESP32-S3-DevKitC-1 (full-size, dual USB-C). See README.md for
 * pin-map rationale, build/flash instructions, and J-Link/OpenOCD setup.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_task_wdt.h"

#include "pins.h"
#include "stepper.h"

static void configureGpio(void)
{
    gpio_config_t latch_cfg = {
        .pin_bit_mask = 1ULL << MOTOR_LATCH_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&latch_cfg);
    gpio_set_level(MOTOR_LATCH_GPIO, 0);

    /* Pull-ups so each button just needs to short its pin to GND when
       pressed, no external resistor - same wiring as the STM32 build. */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_SIM_GPIO) | (1ULL << BTN_RT_GPIO) | (1ULL << BTN_RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
}

static void configureMotorSpi(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = MOTOR_MOSI_GPIO,
        .miso_io_num     = -1,
        .sclk_io_num     = MOTOR_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = sizeof(reg_buffer),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(MOTOR_SPI_HOST, &buscfg, SPI_DMA_DISABLED));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000, /* shift registers care little about speed; 1MHz is plenty */
        .mode           = 0,               /* CPOL=0, CPHA=0 - matches SPI1's mode on the STM32 build */
        .spics_io_num   = -1,              /* no hardware CS; latch is a plain GPIO toggled after the shift */
        .queue_size     = 1,
        .flags          = SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t motor_spi;
    ESP_ERROR_CHECK(spi_bus_add_device(MOTOR_SPI_HOST, &devcfg, &motor_spi));
    Motor_AttachSpiDevice(motor_spi);
}

static void DefaultTaskStart(void *argument)
{
    (void)argument;
    for (;;) {
        gpio_set_level(STATUS_LED_GPIO, !gpio_get_level(STATUS_LED_GPIO));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void MotionTaskStart(void *argument)
{
    (void)argument;
    Motor_InitAll();

    /* Task watchdog: the TWDT is auto-initialized at boot from
       sdkconfig's CONFIG_ESP_TASK_WDT_TIMEOUT_S (4s, matching the STM32
       IWDG). Subscribing this task means a stalled motion loop resets the
       board instead of hanging silently - same safety net, new API. */
    esp_task_wdt_add(NULL);

    bool simPrev = true, rtPrev = true, resetPrev = true; /* idle = HIGH via pull-up */
    uint32_t simEdgeMs = 0, rtEdgeMs = 0, resetEdgeMs = 0;
    const uint32_t DEBOUNCE_MS = 30;

    for (;;) {
        uint32_t nowMs = (uint32_t)(get_micros() / 1000u);

        bool simNow = gpio_get_level(BTN_SIM_GPIO);
        if (simNow != simPrev && (nowMs - simEdgeMs) > DEBOUNCE_MS) {
            simEdgeMs = nowMs;
            simPrev   = simNow;
            if (!simNow) { /* active-low: falling edge = pressed */
                Motor_SetModeSim();
            }
        }

        bool rtNow = gpio_get_level(BTN_RT_GPIO);
        if (rtNow != rtPrev && (nowMs - rtEdgeMs) > DEBOUNCE_MS) {
            rtEdgeMs = nowMs;
            rtPrev   = rtNow;
            if (!rtNow) {
                Motor_SetModeRealtime();
            }
        }

        bool resetNow = gpio_get_level(BTN_RESET_GPIO);
        if (resetNow != resetPrev && (nowMs - resetEdgeMs) > DEBOUNCE_MS) {
            resetEdgeMs = nowMs;
            resetPrev   = resetNow;
            if (!resetNow) {
                Motor_ButtonReset();
            }
        }

        Motor_Poll();

        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void CommTaskStart(void *argument)
{
    (void)argument;
    /* Reserved for the future network link (WiFi/TCP or similar) that
       will feed MODE_REALTIME real ephemeris data. Idle stub for now,
       same as the STM32 build's CommTask. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void app_main(void)
{
    configureGpio();
    configureMotorSpi();

    /* MotionTask is pinned to core 1 and given the highest priority of
       the three so its 1ms poll loop isn't preempted by WiFi/LWIP
       housekeeping (which runs on core 0) once CommTask grows a network
       stack. */
    xTaskCreatePinnedToCore(DefaultTaskStart, "defaultTask", 3072, NULL, 2, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(MotionTaskStart, "MotionTask", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(CommTaskStart, "CommTask", 4096, NULL, 2, NULL, 0);
}
