/**
 * @file portmacro.h（离线测试桩）
 * @brief app_led.c 里 include 了它（为了 portMUX_TYPE），
 *        这里转发到 FreeRTOS.h 的桩。
 */
#ifndef __PORTMACRO_STUB_H__
#define __PORTMACRO_STUB_H__
#include "freertos/FreeRTOS.h"
#endif