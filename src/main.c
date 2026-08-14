#include "esp_err.h"

#include "app/app_init.h"
#include "app/app_runtime.h"

void app_main(void)
{
    ESP_ERROR_CHECK(app_init());
    app_runtime_start();
}
