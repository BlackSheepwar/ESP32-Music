/**
 * @file task.h（离线测试桩）
 * @brief app_led.c 里 include 它（历史上为了 vTaskDelay），
 *        离线测试单线程跑，不需要任务 API。
 */
#ifndef __TASK_STUB_H__
#define __TASK_STUB_H__

#include <stdint.h>
#include "freertos/FreeRTOS.h"

static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }

#endif /* __TASK_STUB_H__ */