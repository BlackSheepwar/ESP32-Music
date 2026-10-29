/**
 * @file task_audio.c
 * @brief 音频任务实现（录音 + 播放统一的实时任务）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本任务现在承担两件事：
 *         1. 建立 I2S 双向通道（麦克风 I2S0 / 扬声器 I2S1）
 *         2. 作为唯一的 I2S 使用者：循环调 App_Audio_Service()
 *            在里面做录音采集和播放推进
 *
 *       ★ 为什么录音和播放不分成两个任务 ★
 *         麦克风和扬声器虽然挂在两个独立 I2S 控制器上，但「同一时刻只做一件事」
 *         让状态机简单得多，也不会出现两个任务同时操作 I2S 的竞态。
 *         代价是录音期间不能播放 —— 但这本来就是要避免的
 *         （一边播一边录会让麦克风收到扬声器的声音，形成回声）。
 *
 *       ★ 自检模式（I2S_LOOPBACK_TEST）已经移到 App_Audio + 按键流程里 ★
 *         原来这里是「录 3 秒 → 打印 → 回放」的固定循环，
 *         现在改成由 KEY3/KEY4 手动触发，更好用：
 *           KEY3 录 5 秒进本地缓冲（可反复做，看出 RMS 是否正常）
 *           KEY4 回放刚录的（不循环）
 *         所以本文件不再需要那段自检代码。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "task_audio.h"
// 功能包含
#include "app_audio.h"
#include "app_audio_cfg.h"
#include "bsp_i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "TASK_AUDIO";

/*==============================================================================
 * 任务函数
 *============================================================================*/
void Task_Audio(void *argument)
{
    (void)argument;

    if (App_Audio_Init() != 0)
    {
        ESP_LOGE(TAG, "App_Audio_Init failed, audio task exit");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio task started (mic I2S0 + spk I2S1, single owner)");

#if I2S_LOOPBACK_TEST
    /* 硬件还没到货时想持续自检可以用这个开关：
     * 录 3 秒 → 打印时长 → 反复进行。
     * 正常使用请保持关闭，改用 KEY3/KEY4 手动测试（更好用：
     * KEY3 录固定 5 秒进本地缓冲，KEY4 回放，不受任务时序约束）。 */
    ESP_LOGW(TAG, "I2S_LOOPBACK_TEST = 1, 进入固定循环自检模式");
    for (;;)
    {
        App_Audio_LoopbackRecordStart(AUDIO_TEST_SECONDS);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_TEST_SECONDS * 1000 + 300));
        size_t got = App_Audio_GetLoopbackFrames();
        ESP_LOGW(TAG, "自检录音 %u 帧 (%.1f s)",
                 (unsigned)got, (double)got / (double)BSP_I2S_SAMPLE_RATE);
    }
#else
    /* 正常路径：每 20ms 进一次服务函数。
     * 20ms 的等待超时是个平衡：
     *   - 太长 → 按键触发的播放命令要等更久才被处理（手感迟钝）
     *   - 太短 → 空转频繁，白耗 CPU
     * 真正的节流来自 i2s_channel_write 阻塞（DMA 只有约 90ms 缓冲），
     * 所以这个超时值只影响"命令响应延迟"，不影响播放速度。 */
    for (;;)
    {
        App_Audio_Service(20);
    }
#endif
}
