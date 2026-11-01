/*
 * paperdiy-reader — firmware entry point for the M5Stack PaperDIY.
 *
 * SPDX-License-Identifier: MIT
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "hal/hal.h"
#include "reader/reader_app.h"

static const char* TAG = "main";

/** The single global HAL instance. */
Hal hal;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "paperdiy-reader starting");

    hal.init();
    hal.settingsLoad();

    // reader_app::run() owns the main task from here on and never returns.
    reader_app::run();

    // Defensive: keep the task alive if run() ever does return.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
