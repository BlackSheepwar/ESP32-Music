/**
 * @file FreeRTOS.h（离线测试桩）
 * @brief 只提供 app_led.c 用到的临界区宏。真机上这些是关中断 + 跨核自旋锁；
 *        单线程的离线测试里退化成空操作即可 —— 我们测的是图案引擎的**逻辑**，
 *        不是它的并发保护（那部分已经单独审过）。
 */
#ifndef __FREERTOS_STUB_H__
#define __FREERTOS_STUB_H__

#include <stdint.h>

typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED  { 0 }

/* 单线程测试：临界区退化为空操作 */
static inline void portENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *m)  { (void)m; }

#endif /* __FREERTOS_STUB_H__ */