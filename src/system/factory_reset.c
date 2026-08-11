#include "system/factory_reset.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led/status_led.h"
#include "node_config.h"

#define BOOT_BUTTON_GPIO 0
#define FACTORY_RESET_HOLD_MS 10000
#define RESET_WARNING_MS 5000

static const char *TAG = "barangay_mesh";

static void init_factory_reset_button(void)
{
    gpio_config_t boot_button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&boot_button_config));
}

static void factory_reset_button_task(void *parameter)
{
    bool warning_blinked = false;
    int held_ms = 0;

    while (true) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;

            if (!warning_blinked && held_ms >= RESET_WARNING_MS) {
                ESP_LOGW(TAG, "BOOT held for 5 seconds. Keep holding for factory reset.");
                status_led_blink_green(1);
                warning_blinked = true;
            }

            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held for 10 seconds. Factory reset confirmed.");
                status_led_blink_green(3);
                node_config_erase();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (held_ms > 0 && held_ms < FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "Factory reset hold cancelled");
            }
            held_ms = 0;
            warning_blinked = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void factory_reset_init(void)
{
    init_factory_reset_button();
    xTaskCreate(factory_reset_button_task, "factory_reset_button_task", 3072, NULL, 7, NULL);
}
