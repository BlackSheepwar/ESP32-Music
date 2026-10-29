/**
 * @file task_wifi.c
 * @brief WiFi 服务任务实现（连接状态机 + 后端探活 + 两个状态指示灯）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本任务本身极薄，但承担了「两个指示灯」的驱动：
 *
 *       WiFi 灯（g_led_wifi / GPIO5）—— 由 APP_WIFI 状态机自己管
 *           灭 = 未开始；快闪 = 连接中；★常亮 = 已连上★；慢闪 = 重连；双闪 = 长退避
 *
 *       后端灯（g_led_backend / GPIO8）—— 由本任务管
 *           灭     = 后端地址没配置，或 WiFi 都没连上（无从谈起）
 *           快闪   = 正在请求 /api/v1/health
 *           常亮   = 后端可达（上一次探活成功）
 *           慢闪   = 后端不可达（连不上/超时/非200）
 *
 *       ★ 后端灯为什么有价值 ★
 *        ★ 已解决：本工程改用自定义域名 www.230508.xyz，国内直连实测可用。
 *        有了这个灯，就能一眼区分三种完全不同的故障：
 *          WiFi 灯灭 + 后端灯灭        → WiFi 都没连上，先修 WiFi
 *          WiFi 灯常亮 + 后端灯慢闪    → 网络通但云端到不了（域名解析错/后端挂了）
 *          WiFi 灯常亮 + 后端灯常亮    → 全链路通
 *        没有这个灯，上面三种情况在串口日志里长得差不多。
 *
 *       ★ 探活会阻塞最长 30 秒（HTTP 超时）★
 *        所以它和 WiFi 状态机放在同一个任务里是合理的：
 *        探活期间 WiFi 事件由 ESP-IDF 自己的事件任务处理，不受影响；
 *        而本任务的周期抖动（30 秒一次）对状态机没有实质影响。
 *        但为了不让探活把状态机饿死，只在「已连接」时才探活。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "task_wifi.h"
// 功能包含
#include "app_led.h"
#include "app_music_app.h"
#include "app_wifi.h"
#include "task_key.h"      /* Task_Key_UploadIsBusy：上传期间暂停探活 */
#include "bsp_http.h"    /* g_http_api + BSP_HTTP_DiagTcp/GetHost */
#include "bsp_wifi.h"    /* g_wifi_sta + BSP_WIFI_SetDns/GetDns */
#include "app_wifi_cfg.h"
#include "app_music_cfg.h"
#include "bsp_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "TASK_WIFI";

/*==============================================================================
 * 后端指示灯
 *============================================================================*/
/**
 * @brief 后端灯的状态（本任务内部用）
 *
 * @note  ★ 为什么要区分 NO_URL 和 OFF ★
 *        最初的实现里，URL 没填时后端灯是「恒灭」—— 但灭跟"这块板子根本没这个灯"
 *        在视觉上完全一样，用户会以为灯坏了或者固件没跑。
 *        现在把两件事分开：
 *          灭        = WiFi 都没连上（后端无从谈起）
 *          快闪      = URL 没填 —— 明确在提示「该去填配置了」
 *          快闪(持续)= 正在探活（和上面同一个图案，但此时串口会打 drain 日志能区分）
 *          常亮      = 可达
 *          慢闪      = 填了但到不了（域名解析错 / 后端挂了 / 网络问题）
 *        这样"快闪"就成了一个明确的动作项提示，而不是静默失败。
 */
typedef enum {
    BE_LED_OFF = 0,     // 灭：WiFi 没连上
    BE_LED_NO_URL,      // 快闪：URL 没填（需要人去改配置）
    BE_LED_BUSY,        // 快闪：正在探活
    BE_LED_OK,          // 常亮：可达
    BE_LED_FAIL,        // 慢闪：不可达
} be_led_state_t;

static be_led_state_t s_be_led = BE_LED_OFF;

/**
 * @brief 设置后端指示灯
 * @note  只在状态变化时才写图案，避免每次探活都重启闪烁相位
 *        （否则快闪看起来会像"卡住不动"）。
 */
static void be_led_set(be_led_state_t st)
{
    if (st == s_be_led)
        return;

    s_be_led = st;

    switch (st)
    {
    case BE_LED_OFF:
        App_LED_Set(APP_LED_BACKEND, &APP_LED_PAT_OFF);
        break;

    case BE_LED_NO_URL:
        /* ★ 快闪提示「配置缺失」★
         * 用快闪而不是别的图案，是因为它最显眼、最像"在等你操作"。
         * 串口同时会打一行中文提示，两边对照就能确认含义。 */
        App_LED_Set(APP_LED_BACKEND, &APP_LED_PAT_FAST_BLINK);
        break;

    case BE_LED_BUSY:
        App_LED_Set(APP_LED_BACKEND, &APP_LED_PAT_FAST_BLINK);
        break;

    case BE_LED_OK:
        App_LED_Set(APP_LED_BACKEND, &APP_LED_PAT_ON);
        break;

    case BE_LED_FAIL:
        App_LED_Set(APP_LED_BACKEND, &APP_LED_PAT_SLOW_BLINK);
        break;
    }
}

/*==============================================================================
 * 任务函数
 *============================================================================*/
void Task_WiFi_Service(void *argument)
{
    (void)argument;

    if (App_WiFi_Init() != 0)
    {
        ESP_LOGE(TAG, "App_WiFi_Init failed, task exit");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "task started, period = %d ms", WIFI_TASK_PERIOD_MS);

    /* ---------- 后端探活初始化 ---------- */
    /* ★★ 不要在启动时把"URL 是否已配置"缓存成常量（这是一个真实 bug）★★
     *
     * 原来的写法是：
     *     uint8_t backend_configured = App_MusicApp_IsBackendConfigured();   ← 只读一次
     * 然后整个任务的生命周期都依赖这一个值。但本任务是在 main.c:156 创建的，
     * 而 App_MusicApp_Init()（它才把 URL 装进 HTTP 层）在 main.c:189 才调用：
     *
     *     main.c:156  xTaskCreatePinnedToCore(Task_WiFi_Service, ...)
     *     main.c:189  App_MusicApp_Init()
     *
     * 实测启动顺序完全印证了这一点：
     *     I (1283) TASK_WIFI: task started, period = 50 ms
     *     W (1294) TASK_WIFI: backend probe disabled: BACKEND_BASE_URL not configured
     *     ...
     *     I (1365) BSP_HTTP: base url = https://www.230508.xyz      ← 迟到了 70ms
     *     I (1375) APP_MUSIC: backend configured: https://www.230508.xyz
     *
     * 后果：**探活被永久禁用** —— 不只是后端指示灯不工作，
     * 连失败时的 DNS/TCP 分层诊断（BACKEND_DIAG_ON_FAIL）也一起失效了，
     * 而那正是排查网络问题最重要的工具。
     *
     * 修法：不缓存，改成每轮 Service 实时查询。
     *   · App_MusicApp_IsBackendConfigured() 只是读一个已初始化的指针，极廉价
     *   · 这样无论将来初始化顺序怎么调整都不会再踩这个坑
     *   · 而且支持"运行期后来才配置"的场景（比如以后从 NVS 读 URL） */
    uint32_t next_probe_ms = 0;
    uint32_t probe_count = 0;

    /* ★ URL 没配置时：一旦 WiFi 连上就让后端灯开始快闪 ★
     * 这是一个动作项提示（"该去填 app_music_cfg.h 了"），而不是静默的灭。
     * 用 url_warned 保证那几行中文提示只打一次，不刷屏。 */
    uint8_t  url_warned = 0;

    /* 启动时打印一次当前配置状态（只是日志，不作为后续判断依据） */
#if BACKEND_PROBE_ENABLE
    if (App_MusicApp_IsBackendConfigured())
        ESP_LOGI(TAG, "backend probe will start after WiFi connects (every %d ms)",
                 BACKEND_PROBE_PERIOD_MS);
    else
        ESP_LOGW(TAG, "backend probe: BACKEND_BASE_URL not configured YET "
                      "(will re-check every cycle; see note in task_wifi.c)");
    be_led_set(BE_LED_OFF);     /* WiFi 还没连上，先灭 */
#else
    be_led_set(BE_LED_OFF);
    ESP_LOGI(TAG, "backend probe disabled by config (BACKEND_PROBE_ENABLE = 0)");
#endif

    uint8_t was_connected = 0;

    for (;;)
    {
        /* ---------- 1. 推进 WiFi 状态机 ---------- */
        App_WiFi_Service();

        uint8_t now_connected = App_WiFi_IsConnected();

        /* ---------- 2. WiFi 刚连上 / 刚掉线时的处理 ---------- */
        if (now_connected && !was_connected)
        {
            ESP_LOGI(TAG, "WiFi connected: ip=%s rssi=%d dBm",
                     App_WiFi_GetIP(), (int)App_WiFi_GetRssi());

            /* ---------- DNS 覆盖（可选，用来排除 DNS 污染）----------
             * 必须在拿到 IP 之后设：DHCP 完成前 netif 还没配好 IP，
             * 那时写 DNS 会被 DHCP 的结果覆盖掉。 */
#if BACKEND_DNS_OVERRIDE_ENABLE
            BSP_WIFI_SetDns(g_wifi_sta, BACKEND_DNS_PRIMARY, BACKEND_DNS_SECONDARY);
#endif
            {
                char dns1[16] = {0}, dns2[16] = {0};
                if (BSP_WIFI_GetDns(g_wifi_sta, dns1, dns2) == 0)
                {
                    ESP_LOGI(TAG, "DNS: 主=%s 备=%s%s", dns1, dns2,
                             (BACKEND_DNS_OVERRIDE_ENABLE) ? " (已覆盖)" : " (来自 DHCP)");
                }
            }

            /* ★ 实时查询（不再用启动时缓存的 backend_configured）★ */
            if (!App_MusicApp_IsBackendConfigured())
            {
                /* ★ URL 没填 → 快闪提示 ★ */
                be_led_set(BE_LED_NO_URL);
                if (!url_warned)
                {
                    url_warned = 1;
                    ESP_LOGE(TAG, "==================================================");
                    ESP_LOGE(TAG, " 后端指示灯正在快闪 = BACKEND_BASE_URL 还没填！");
                    ESP_LOGE(TAG, " 打开 User/APP/APP_MUSIC/app_music_cfg.h，把");
                    ESP_LOGE(TAG, "   #define BACKEND_BASE_URL \"https://www.230508.xyz\"");
                    ESP_LOGE(TAG, " （本工程当前是自定义域名，国内直连实测可用）");
                    ESP_LOGE(TAG, " 填好重新烧录后，这个灯会变成：");
                    ESP_LOGE(TAG, "   慢闪 = 填了但连不上（先怀疑网络环境）");
                    ESP_LOGE(TAG, "   常亮 = 全链路通");
                    ESP_LOGE(TAG, "==================================================");
                }
            }
#if BACKEND_PROBE_ENABLE
            else
            {
                next_probe_ms = BSP_SYS_GetTickMs() + 500;  /* 连上后 0.5 秒开始探活 */
                be_led_set(BE_LED_BUSY);
                ESP_LOGI(TAG, "首次后端探活将在 500ms 后进行...");
            }
#endif
        }
        else if (!now_connected && was_connected)
        {
            ESP_LOGW(TAG, "WiFi lost, backend LED -> OFF");
            be_led_set(BE_LED_OFF);     /* WiFi 都断了，后端无从谈起 */
            next_probe_ms = 0;
        }

        was_connected = now_connected;

        /* ---------- 3. 后端探活 ---------- */
#if BACKEND_PROBE_ENABLE
        /* ★ 每轮实时查询"URL 是否已配置"（不再用启动时那个缓存值）★
         *   缓存它曾经导致探活被永久禁用 —— 因为本任务比
         *   App_MusicApp_Init() 先启动。详见本文件靠上位置的详细说明。 */
        if (!App_MusicApp_IsBackendConfigured() && now_connected && !url_warned)
        {
            /* URL 还没就绪（可能只是初始化顺序问题）→ 提示一次 */
            be_led_set(BE_LED_NO_URL);
            url_warned = 1;
            ESP_LOGE(TAG, "==================================================");
            ESP_LOGE(TAG, " 后端指示灯快闪 = BACKEND_BASE_URL 还没就绪");
            ESP_LOGE(TAG, " 检查 User/APP/APP_MUSIC/app_music_cfg.h 里的");
            ESP_LOGE(TAG, "   #define BACKEND_BASE_URL \"https://www.230508.xyz\"");
            ESP_LOGE(TAG, " （本工程当前是自定义域名，国内直连实测可用）");
            ESP_LOGE(TAG, " 正常时这个灯：慢闪 = 连不上，常亮 = 全链路通");
            ESP_LOGE(TAG, "==================================================");
        }

        /* ★★ 上传期间【完全暂停】探活（实测优化）★★
         *
         * 实测日志：上传 88364 字节期间，探活仍然每 30 秒跑一次
         *     I (520688) POST ... (88364 bytes upload)
         *     I (524203) 后端探活 #12 ...      ← 上传开始后 3.5 秒就来了
         *     I (554203) 后端探活 #13 ...
         *     I (584203) 后端探活 #14 ...
         *
         * 探活虽然拿不到 HTTP 互斥锁会跳过（已在 app_music 侧做了 trylock），
         * 但它【每次都会走一遍 DNS + TLS 连接建立】—— 和上传抢同一条链路。
         * 在本来就不稳定的链路上，额外建立并发连接只会增加失败概率。
         *
         * 所以上传期间连"尝试"都不做：直接跳过，等上传结束后再恢复。
         * 代价只是后端指示灯晚几十秒更新，完全可接受。
         *
         * ★ 注意 next_probe_ms 不推进 ★
         *   这样上传一结束就会立刻补探一次（条件仍然成立），不会白等 30 秒。 */
        if (Task_Key_UploadIsBusy())
        {
            /* 上传中：本轮什么都不做（连 DNS 都不查） */
        }
        else if (App_MusicApp_IsBackendConfigured() && now_connected &&
            next_probe_ms != 0 &&
            (int32_t)(BSP_SYS_GetTickMs() - next_probe_ms) >= 0)
        {
            probe_count++;
            be_led_set(BE_LED_BUSY);

            ESP_LOGI(TAG, "后端探活 #%u ...", (unsigned)probe_count);
            int r = App_MusicApp_ProbeBackend();

            /* ★ 区分"跳过"和"失败"（审计修复 M9）★
             * 探活拿不到后端访问权时会返回 APP_MUSIC_ERR_BUSY_SKIP ——
             * 那说明后端正被上传占用，不是连不上。
             * 这种情况【绝对不能】把灯设成快闪"不可达"，否则会出现
             * "后端明明好好的、指示灯却报故障"的假警报。
             * 保持上一次的灯状态，等下一个 30 秒周期再探。 */
            if (r == APP_MUSIC_ERR_BUSY_SKIP)
            {
                ESP_LOGD(TAG, "探活本轮跳过（后端正忙），指示灯保持不变");
            }
            else
            {
                be_led_set((r == 0) ? BE_LED_OK : BE_LED_FAIL);
            }

#if BACKEND_DIAG_ON_FAIL
            /* ★ 失败时做一轮分层诊断：把“连不上”拆成 DNS / TCP / TLS 三层 ★
             * 这三层的修法完全不同，不分开就只能反复猜。
             * 注意要排除 BUSY_SKIP：那不是失败，做诊断没有意义。 */
            if (r != 0 && r != APP_MUSIC_ERR_BUSY_SKIP)
            {
                char host[128] = {0};
                char ip[16]    = {0};

                ESP_LOGW(TAG, "---------------- 网络分层诊断 ----------------");
                if (BSP_HTTP_GetHost(g_http_api, host, sizeof(host)) == 0)
                {
                    int dr = BSP_HTTP_DiagTcp(g_http_api, host, 443, 8000, ip);

                    if (dr == -2)
                        ESP_LOGE(TAG, "  结论：DNS 解析失败 -> 换 DNS 服务器（见 app_music_cfg.h）");
                    else if (dr == -3)
                        ESP_LOGE(TAG, "  结论：DNS 给出 %s 但 TCP 连不通", ip);
                    else if (dr == 0)
                        ESP_LOGI(TAG, "  结论：TCP 通，问题在 TLS/服务端");
                }
                else
                {
                    ESP_LOGE(TAG, "  无法从 URL 解析主机名");
                }
                ESP_LOGW(TAG, "  若 TCP 层就不通，说明是网络环境问题，固件改不了；");
                ESP_LOGW(TAG, "  先试手机热点，或改用自定义域名（见 HARDWARE.md 3.2）");
                ESP_LOGW(TAG, "----------------------------------------------");
            }
#endif

            next_probe_ms = BSP_SYS_GetTickMs() + BACKEND_PROBE_PERIOD_MS;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(WIFI_TASK_PERIOD_MS));
    }
}
