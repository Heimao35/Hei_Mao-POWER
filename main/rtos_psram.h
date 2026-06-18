/**
 * @file rtos_psram.h
 * @brief 在 PSRAM 中分配 FreeRTOS 任务栈（TCB 仍在内部 RAM）。
 */
#ifndef RTOS_PSRAM_H
#define RTOS_PSRAM_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 创建任务：优先 PSRAM 栈 + 内部 TCB；失败时回退 xTaskCreate（内部 RAM）。
 * 成功时栈/TCB 由 RTOS 持有，任务删除后也不释放（适用于长生命周期 worker）。
 */
TaskHandle_t rtos_task_create_psram(TaskFunction_t fn,
                                    const char    *name,
                                    uint32_t       stack_depth,
                                    void          *arg,
                                    UBaseType_t    prio);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_PSRAM_H */
