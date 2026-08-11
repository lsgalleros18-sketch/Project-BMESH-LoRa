#include "led/status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RGB_LED_GPIO 48

static const char *TAG = "barangay_mesh";
static rmt_channel_handle_t rgb_led_channel;
static rmt_encoder_handle_t rgb_led_encoder;
static bool rgb_led_ready;

static void rgb_led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t grb_data[3] = {green, red, blue};
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    if (!rgb_led_ready) {
        return;
    }

    rmt_transmit(rgb_led_channel, rgb_led_encoder, grb_data, sizeof(grb_data), &transmit_config);
    rmt_tx_wait_all_done(rgb_led_channel, pdMS_TO_TICKS(100));
}

void status_led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };

    if (rmt_new_tx_channel(&channel_config, &rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed on GPIO%d", RGB_LED_GPIO);
        return;
    }

    if (rmt_new_bytes_encoder(&encoder_config, &rgb_led_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED encoder init failed");
        return;
    }

    if (rmt_enable(rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED channel enable failed");
        return;
    }

    rgb_led_ready = true;
}

void status_led_blink_green(int count)
{
    for (int i = 0; i < count; i++) {
        rgb_led_set(0, 48, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        rgb_led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
