/**
 * @file rtos_psram.c
 */
#include "rtos_psram.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>

static const char *TAG = "rtos_psram";

TaskHandle_t rtos_task_create_psram(TaskFunction_t fn,
                                      const char    *name,
                                      uint32_t       stack_depth,
                                      void          *arg,
                                      UBaseType_t    prio)
{
    if (!fn || !name || stack_depth == 0) {
        return NULL;
    }

    StackType_t *stack = (StackType_t *)heap_caps_malloc(
        (size_t)stack_depth * sizeof(StackType_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);

    if (stack && tcb) {
        TaskHandle_t h = xTaskCreateStatic(fn, name, stack_depth, arg, prio, stack, tcb);
        if (h) {
            ESP_LOGI(TAG, "task %s: PSRAM stack %lu words", name, (unsigned long)stack_depth);
            return h;
        }
    }

    free(stack);
    free(tcb);

    TaskHandle_t h = NULL;
    if (xTaskCreate(fn, name, stack_depth, arg, prio, &h) == pdPASS) {
        ESP_LOGW(TAG, "task %s: PSRAM stack failed, using internal RAM", name);
        return h;
    }

    ESP_LOGE(TAG, "task %s: create failed", name);
    return NULL;
}
