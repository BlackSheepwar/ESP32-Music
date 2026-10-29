/**
 * @file task_led.c
 * @brief LED 任务实现（LED 服务 + 生命灯）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 两个任务放在一个文件里，因为它们操作的是同一批硬件（GPIO 指示灯），
 *       拆成两个文件反而看不出「谁负责哪些灯」。分工是明确的：
 *
 *         Task_LED_Service  → 4 个语义灯（WiFi / 录音 / 音频 / 后端），10ms 状态机
 *         Task_Heartbeat    → 生命灯（1 个），500ms 纯翻转
 *
 *       FreeRTOS tick 是 1000Hz（sdkconfig 的 CONFIG_FREERTOS_HZ=1000），
 *       所以 pdMS_TO_TICKS(500) = 500 tick，方波周期非常准。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "task_led.h"
// 功能包含
#include "app_led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "TASK_LED";

/*==============================================================================
 * 任务函数：LED 服务
 *============================================================================*/
void Task_LED_Service(void *argument)
{
    (void)argument;

    /* 先初始化：配置指示灯引脚 + 填点位表 + 全部置灭 */
    App_LED_Init();

    ESP_LOGI(TAG, "led service started, period = %d ms", APP_LED_TICK_MS);

    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        App_LED_Service();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_LED_TICK_MS));
    }
}

/*==============================================================================
 * 任务函数：生命灯
 *============================================================================*/
void Task_Heartbeat(void *argument)
{
    (void)argument;

    /* ★ 不在这里调 App_LED_Init()：那会重复配置引脚，而且依赖 LED 任务先跑。
     *   生命灯的翻转在 App_LED_HeartbeatToggle() 内部自己取点位表的句柄，
     *   如果 LED 任务还没初始化完，那个句柄是 NULL，函数会直接返回（不崩）。 */

#if APP_LED_HB_SEPARATE_TASK
    /* 等 LED 任务把点位表填好（最坏情况等 100ms，肉眼无感） */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "heartbeat started: GPIO toggle every %d ms", APP_LED_HB_PERIOD_MS);

    for (;;)
    {
        App_LED_HeartbeatToggle();
        vTaskDelay(pdMS_TO_TICKS(APP_LED_HB_PERIOD_MS));
    }
#else
    /* 由 LED 状态机走图案，本任务不存在 */
    ESP_LOGW(TAG, "heartbeat task disabled (APP_LED_HB_SEPARATE_TASK = 0)");
    vTaskDelete(NULL);
#endif
}
