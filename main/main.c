#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// ----- MACROS ----- //
#define ets_delay_us esp_rom_delay_us // Patch ets_delay_us call
#define HALL_GPIO GPIO_NUM_0
#define ESP_INTR_FLAG_DEFAULT 0
#define NUM_SLICES 14   // number of slices for POV display
// ------------------ //

// TAG for LOGS
static const char *TAG = "POV";

// Queue for intercom between ISR and task
static QueueHandle_t hall_queue = NULL;

// Latest per-slice delay (ΔT for POV)
static volatile uint32_t t_slice_us_global = 0;

// ISR for Hall effect sensor
static void IRAM_ATTR hall_isr_handler(void* arg)
{
    int64_t now_us = esp_timer_get_time();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // overwrite single-slot queue with latest timestamp
    xQueueOverwriteFromISR(hall_queue, &now_us, &xHigherPriorityTaskWoken);

    // if hall task is higher priority, yield immediately
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// High-priority task: computes T_rev and t_slice from timestamps
static void hall_task(void* arg)
{
    int64_t now_us;
    int64_t prev_us = 0;
    uint32_t T_rev_us;   // full wheel revolution period
    uint32_t t_slice_us; // per-slice delay

    while (1) {
        if (xQueueReceive(hall_queue, &now_us, portMAX_DELAY)) {
            if (prev_us != 0) {
                // Compute full revolution time
                T_rev_us = (uint32_t)(now_us - prev_us);

                // Compute per-slice delay for POV
                t_slice_us = T_rev_us / NUM_SLICES;

                // Save globally so POV task can use it
                t_slice_us_global = t_slice_us;

                ESP_LOGI(TAG, "T_rev = %lu µs, t_slice = %lu µs, freq = %.2f Hz",
                         T_rev_us, t_slice_us, 1e6 / (double)T_rev_us);
            }
            prev_us = now_us;
        }
    }
}

// Medium-priority task: drives POV display based on latest t_slice
static void pov_task(void* arg)
{
    uint32_t t_slice_local;

    while(1) {
        // Copy the latest per-slice delay to local variable
        t_slice_local = t_slice_us_global;

        if (t_slice_local == 0) {
            // Wait until first revolution is measured
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (int slice = 0; slice < NUM_SLICES; slice++) {
            // TODO: send slice[slice] colors to WLED strip
            // Example placeholder:
            // send_slice_to_wled(slice);

            // Wait for per-slice duration
            ets_delay_us(t_slice_local);
        }
    }
}

void app_main(void)
{
    // Create single-slot queue for Hall timestamps
    hall_queue = xQueueCreate(1, sizeof(int64_t));

    // Configure GPIO for Hall effect sensor
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = 1ULL << HALL_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    // Install ISR service and attach handler
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(HALL_GPIO, hall_isr_handler, NULL);

    // Start tasks
    // Hall task: higher priority for accurate ΔT computation
    xTaskCreate(hall_task, "hall_task", 2048, NULL, 10, NULL);
    // POV display task: medium priority
    xTaskCreate(pov_task, "pov_task", 4096, NULL, 5, NULL);
}
