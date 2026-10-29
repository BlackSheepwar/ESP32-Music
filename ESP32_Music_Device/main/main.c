/**
 * @file main.c
 * @brief 主程序（应用入口）
 * @author 李嘉图
 *
 * @note ★ 分层约定：main.c 只做三件事 ★
 *         1. 打印芯片信息（确认板子是不是 N16R8、PSRAM 有没有起来）
 *         2. 初始化各 APP 层模块（按依赖顺序）
 *         3. 创建各层的 FreeRTOS 任务，自己退化成「监控任务」
 *
 *       所有硬件动作都在 BSP 层、所有业务判断都在 APP 层，
 *       main.c 里不出现任何 GPIO/I2S/WiFi/HTTP 的具体调用 —— 这是分层底线。
 *
 *==============================================================================
 * 任务清单（名称 / 优先级 / 周期 / 职责）
 *==============================================================================
 *   task_audio     6   事件驱动   唯一操作 I2S：录音采集 + 播放推进（硬实时）
 *   task_key_cmd   5   事件驱动   按键事件分发 → 录音/上传/播放（会阻塞在 HTTP）
 *   task_key       5   10ms       按键扫描 + 录音状态灯巡检
 *   task_wifi      4   50ms       WiFi 状态机 + 后端探活 + 后端指示灯
 *   task_led       1   10ms       4 个语义灯的闪烁相位推进
 *   task_heartbeat 1   500ms      生命灯翻转（纯延时+翻转，不做任何判断）
 *
 * ★ 为什么音频优先级最高：I2S DMA 缓冲只有约 90ms，读晚了就 overflow
 *   丢样点（听到「咔咔」爆音）。灯晚 10ms 没人看得出来，音频晚 10ms 就废了。
 *
 * ★ 生命灯为什么单独一个任务、优先级还给最低：
 *   它的意义是"证明系统还活着"。刻意只做 vTaskDelay + 翻转，
 *   没有状态机、没有时间比较、没有日志 —— 代码路径越短越不可能自己出错，
 *   于是"它不闪了"才真的等价于"系统出问题了"。
 *   优先级最低保证其它任务正常时它不被饿死；
 *   如果连它都跑不到，那系统确实已经出大问题。
 *
 *   ⚠️ 它能覆盖的故障范围：「调度器还在跑，但某个任务卡住了」。
 *      覆盖不了「全局死锁 / 中断异常」—— 那需要硬件看门狗（esp_task_wdt），
 *      而本工程有 30 秒的阻塞式 HTTP，现在加会误触发。
 *      产品化时再把 HTTP 改成异步 + 开看门狗。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdio.h>
#include "ds_common.h"
// 功能包含
#include "app_led.h"
#include "app_ledtest.h"
#include "app_music_app.h"
#include "bsp_sys.h"
#include "task_audio.h"
#include "task_key.h"
#include "task_led.h"
#include "task_wifi.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "MAIN";

/** @brief 各任务的栈大小（字节）。ESP-IDF 的栈按字节算，不是 word
 *
 *  ★ STACK_KEY_CMD 从 4096 提到 8192（审计修复）★
 *    Task_Key_Cmd 不只是分发按键，它还同步执行整条上传链路：
 *      esp_http_client_perform()（TLS 握手在【调用者上下文】里跑）
 *      + 两次 cJSON_Parse（每次都要在栈上递归遍历语法树）
 *    而 WiFi 探活任务（同样做 TLS）我当初给了 6144 并注明"HTTP/TLS 栈需求大"，
 *    却给负载更重的上传路径只留了 4096 —— 这是明显的不一致。
 *
 *    sdkconfig 里 CONFIG_ESP_SYSTEM_CHECK_STACKOVERFLOW_CANARY=1 只检查栈底哨兵，
 *    溢出会【静默】破坏相邻的堆内存，症状是莫名其妙的崩溃或数据错乱，极难定位。
 *    所以宁可多给 4KB（内部 RAM 还有 215KB 空闲，完全负担得起）。
 *
 *  ★ 实测建议 ★
 *    跑通上传后可以用 uxTaskGetStackHighWaterMark(NULL) 在任务里
 *    打印剩余栈高水位，确认真实峰值后再决定要不要回收这 4KB。 */
#define STACK_AUDIO     4096
#define STACK_KEY_CMD   8192
/* ★ 上传 worker 的栈：和 task_key_cmd 同一量级 ★
 *   它跑的是原来 task_key_cmd 里那条最重的路径：
 *   esp_http_client_perform（TLS 握手在调用者上下文）+ cJSON_Parse
 *   + KS_Synth_Init + WAV 打包。所以栈必须给足，不能比 8192 小。
 *
 *   ★ 为什么要单独开一个任务 ★
 *   上传最坏要 3.75 分钟，放在 task_key_cmd 里会让它长期不消费按键事件
 *   队列（深度 16）→ 溢出丢事件 → 按键状态机与上层记录永久不一致。
 *   实测症状：「第一次按 KEY1 能录，第二次按 KEY1 没反应」。 */
#define STACK_KEY_UPLOAD 8192
#define STACK_KEY_SCAN  3072
/* ★ STACK_WIFI 从 6144 提到 8192（审计修复 M4）★
 * 它不只是 WiFi 状态机：探活链路的 esp_http_client_perform（TLS 握手）、
 * cJSON_Parse、以及失败诊断里的 getaddrinfo() 都在这个任务上下文里跑。
 * sdkconfig 里 MBEDTLS_SSL_IN_CONTENT_LEN=16384 且关闭了变长缓冲，
 * 握手状态机 + x509 证书链校验在栈上开销不小，IDF 的 HTTPS 示例通常给 8192。
 * CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1 只能查栈底哨兵，
 * 溢出会静默破坏相邻堆内存 —— 内部 RAM 还有 215KB 空闲，没必要省这 2KB。 */
#define STACK_WIFI      8192
#define STACK_LED       2560
#define STACK_HEARTBEAT 2048
#define STACK_LEDTEST   3072

/** @brief 各任务的优先级（数字越大越高） */
#define PRIO_AUDIO      6
#define PRIO_KEY_CMD    5
#define PRIO_KEY_SCAN   5
#define PRIO_WIFI       4
#define PRIO_LED        1
#define PRIO_HEARTBEAT  1
#define PRIO_LEDTEST    2       /* 仅 LED 自检打开时有意义 */

/**
 * @brief 任务绑核
 * @note  ESP32-S3 双核：PRO_CPU = 0，APP_CPU = 1。
 *        WiFi 协议栈默认在 core 0，探活也放 core 0（同一个任务里）。
 *        音频、按键、灯放 core 1，避免 WiFi 收发时的长时间占用把 I2S 读取挤掉。
 */
#define CORE_AUDIO      1
#define CORE_KEY        1
#define CORE_WIFI       0
#define CORE_LED        1

/** @brief 内存水位打印周期（毫秒） */
#define MEM_LOG_PERIOD_MS   30000

/*==============================================================================
 * 主函数
 *============================================================================*/
void app_main(void)
{
    /* ---------- 1. 芯片信息 ---------- */
    /* 重点看两行：Flash 是不是 16MB、PSRAM 是不是 8MB 且 initialized。
     * PSRAM 没起来的话录音缓冲（256KB）和合成器（66KB）会分配失败。 */
    BSP_SYS_PrintChipInfo();

    ESP_LOGI(TAG, "==== ESP32 Music Device booting ====");
    ESP_LOGI(TAG, "IDF %s, FreeRTOS tick = %d Hz",
             esp_get_idf_version(), configTICK_RATE_HZ);

    /* ---------- 2. LED 必须先初始化 ----------
     * 后面 APP_WIFI / APP_MUSIC 初始化时就会去点灯。
     * 如果那时点位表还没填，App_LED_Set 会静默失败（句柄为 NULL），
     * 表现为"灯该亮却不亮"，而且日志里看不出原因。
     * ★ 这就是"先初始化硬件抽象，再初始化业务"的价值 ★ */
    App_LED_Init();

    /* ---------- 3. 创建任务 ---------- */
    BaseType_t ret;

    /* 生命灯：最快让用户看到"设备活着"，所以第一个建 */
    ret = xTaskCreatePinnedToCore(Task_Heartbeat, "task_heartbeat",
                                  STACK_HEARTBEAT, NULL, PRIO_HEARTBEAT, NULL, CORE_LED);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_heartbeat failed");

    /* LED 服务：推进 4 个语义灯的闪烁图案（生命灯不归它管） */
    ret = xTaskCreatePinnedToCore(Task_LED_Service, "task_led",
                                  STACK_LED, NULL, PRIO_LED, NULL, CORE_LED);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_led failed");

    /* WiFi 服务：初始化 BSP_WIFI + 状态机 + 后端探活（首次 Service 会自动开始连） */
    ret = xTaskCreatePinnedToCore(Task_WiFi_Service, "task_wifi",
                                  STACK_WIFI, NULL, PRIO_WIFI, NULL, CORE_WIFI);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_wifi failed");

    /* 音频：建立 I2S 双向通道（麦克风 I2S0 + 扬声器 I2S1）*/
    ret = xTaskCreatePinnedToCore(Task_Audio, "task_audio",
                                  STACK_AUDIO, NULL, PRIO_AUDIO, NULL, CORE_AUDIO);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_audio failed");

    /* 按键扫描（生产者）+ 命令分发（消费者）*/
    ret = xTaskCreatePinnedToCore(Task_Key_Debounce, "task_key",
                                  STACK_KEY_SCAN, NULL, PRIO_KEY_SCAN, NULL, CORE_KEY);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_key failed");

    ret = xTaskCreatePinnedToCore(Task_Key_Cmd, "task_key_cmd",
                                  STACK_KEY_CMD, NULL, PRIO_KEY_CMD, NULL, CORE_KEY);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_key_cmd failed");

    /* LED 自检（默认关闭）：打开后扫描所有引脚定位"灯接在哪个脚上"。
     * 这是诊断工具，逻辑在 APP 层，所以直接调它的任务函数，不额外包装。 */
    ret = xTaskCreatePinnedToCore(Task_LED_Test, "task_ledtest",
                                  STACK_LEDTEST, NULL, PRIO_LEDTEST, NULL, CORE_LED);
    if (ret != pdPASS)
        ESP_LOGE(TAG, "create task_ledtest failed");

    /* 音乐业务：BSP_HTTP + 大块 PSRAM（响应缓冲 / music_t / KS 合成器）
     * ★ 放在最后，且在 WiFi 任务建好之后 ★
     *   这样它的分配发生在 WiFi 已经占好内存之后，
     *   不会出现"业务先吃掉 PSRAM、WiFi 起来时反而紧"的情况。 */
    if (App_MusicApp_Init() != 0)
        ESP_LOGE(TAG, "App_MusicApp_Init failed (上传功能不可用，其余功能正常)");

    /* ★ 上传 worker：把耗时上传从按键任务里搬出来 ★
     *   必须在 Task_Key_Cmd 之后创建（后者会建上传信号量），
     *   但即使顺序反了也没关系 —— worker 在信号量为 NULL 时会自己退出并报错。 */
    ret = xTaskCreatePinnedToCore(Task_Key_Upload, "task_key_upload",
                                  STACK_KEY_UPLOAD, NULL, 5, NULL, CORE_KEY);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "create task_key_upload failed: %d", (int)ret);
    }
    else
    {
        ESP_LOGI(TAG, "  task_key_upload: KEY1 上传由它执行（不阻塞按键分发）");
    }

    ESP_LOGI(TAG, "all tasks created");
    ESP_LOGI(TAG, "操作说明: KEY1=按住录音/松手上传   KEY2=播放合成(循环)/停止");
    ESP_LOGI(TAG, "          KEY3=回环录音5秒(本地)   KEY4=回放回环(单次)/停止");

    /* ---------- 4. 主任务退化成监控 ---------- */
    /* 不做业务，只周期性报告内存水位，方便判断加 HTTPS/cJSON/KS 合成之后
     * 有没有内存泄漏。internal 那一项低于 20KB 就要警惕了。 */
    for (;;)
    {
        ESP_LOGI(TAG, "heap: internal free = %u B (min %u B), PSRAM free = %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        vTaskDelay(pdMS_TO_TICKS(MEM_LOG_PERIOD_MS));
    }
}
