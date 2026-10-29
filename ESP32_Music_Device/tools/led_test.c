/**
 * @file led_test.c
 * @brief LED 图案引擎离线验证（PC 上跑，不需要硬件）
 *
 * @note  ★ 要解决的问题 ★
 *        真机现象：手机热点关掉 → 串口证明状态机切到 RETRY_WAIT 并调了
 *        App_LED_Set(APP_LED_WIFI, PAT_SLOW_BLINK)，但 GPIO5 物理上完全不闪、
 *        一直常亮。
 *
 *        在真机上无法区分：
 *          a) App_LED_Set 没被调到 / 被早退拦住
 *          b) 图案引擎算出的电平一直是"亮"
 *        这个程序直接驱动**真实的 app_led.c**，把每个 tick 算出来的电平
 *        记录下来，看它到底有没有在 ON/OFF 之间翻转。
 *
 * ★ 编译方式见 tools/run_led_test.ps1 ★
 * 本文件不属于固件，ESP-IDF 不会编译它。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "bsp_gpio.h"
#include "app_led.h"
#include "app_led_cfg.h"

esp_log_level_t g_log_level = ESP_LOG_WARN;

/*============================================================================
 * 假句柄：app_led.c 只把指针传下去、不 dereference（真正的引脚知识在
 * bsp_gpio.c 里），所以这里给每个灯一个非 NULL 的哨兵指针就够。
 * 用指针值当"引脚号"来记录，方便断言。
 *============================================================================*/
#define PIN_HB      4
#define PIN_WIFI    5
#define PIN_MIC     6
#define PIN_AUDIO   7
#define PIN_BACKEND 8
#define PIN_SPARE   9

/* app_led.c 直接引用这些具名句柄，所以要提供定义 */
const BSP_GPIO_Handle_t *const g_led_hb      = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_HB;
const BSP_GPIO_Handle_t *const g_led_wifi    = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_WIFI;
const BSP_GPIO_Handle_t *const g_led_mic     = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_MIC;
const BSP_GPIO_Handle_t *const g_led_audio   = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_AUDIO;
const BSP_GPIO_Handle_t *const g_led_backend = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_BACKEND;
const BSP_GPIO_Handle_t *const g_led_spare   = (const BSP_GPIO_Handle_t *)(uintptr_t)PIN_SPARE;

/*============================================================================
 * BSP_GPIO 打桩：记录每次电平写入
 *============================================================================*/
#define MAX_LOG 8192
static struct { uint8_t pin; uint8_t level; uint32_t tick; } g_wr[MAX_LOG];
static int      g_wr_count = 0;
static uint32_t g_tick     = 0;
static uint8_t  g_pin_level[64];

static void wr(uint8_t pin, uint8_t lv)
{
    if (g_wr_count < MAX_LOG)
    {
        g_wr[g_wr_count].pin   = pin;
        g_wr[g_wr_count].level = lv;
        g_wr[g_wr_count].tick  = g_tick;
        g_wr_count++;
    }
    if (pin < 64) g_pin_level[pin] = lv;
}

void BSP_GPIO_Init(void) { }
void BSP_GPIO_InitOutputs(void) { }
void BSP_GPIO_InitInputs(void) { }

uint8_t BSP_GPIO_SetLevel(const BSP_GPIO_Handle_t *h, BSP_GPIO_Level_t level)
{
    if (h == NULL) return 0;
    wr((uint8_t)(uintptr_t)h, (level == GPIO_LEVEL_HIGH) ? 1 : 0);
    return 1;
}

uint8_t BSP_GPIO_ReadLevel(const BSP_GPIO_Handle_t *h)
{
    if (h == NULL) return 0xFF;
    uint8_t p = (uint8_t)(uintptr_t)h;
    return (p < 64) ? g_pin_level[p] : 0xFF;
}

uint8_t BSP_GPIO_Toggle(const BSP_GPIO_Handle_t *h)
{
    if (h == NULL) return 0;
    uint8_t p = (uint8_t)(uintptr_t)h;
    wr(p, g_pin_level[p] ? 0 : 1);
    return 1;
}

uint8_t BSP_GPIO_IsOutput(const BSP_GPIO_Handle_t *h) { return h ? 1 : 0; }
uint8_t BSP_GPIO_IsInput(const BSP_GPIO_Handle_t *h)  { return h ? 1 : 0; }
uint8_t BSP_GPIO_GetNum(const BSP_GPIO_Handle_t *h)   { return h ? (uint8_t)(uintptr_t)h : 0xFF; }
int     BSP_GPIO_PulsePinTemporary(int pin, BSP_GPIO_Level_t lv, uint32_t hold)
{ (void)pin; (void)lv; (void)hold; return 0; }

uint32_t BSP_SYS_GetTickMs(void) { return g_tick; }

/*============================================================================
 * 断言与辅助
 *============================================================================*/
static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  ✓ %s\n", what); }
    else      { g_fail++; printf("  ✗ FAIL: %s\n", what); }
}

/** 跑 n 个 tick（每 tick = 10ms，调一次 App_LED_Service） */
static void run_ticks(int n)
{
    for (int i = 0; i < n; i++) { g_tick++; App_LED_Service(); }
}

/** 统计某引脚在 [from,to) tick 内的电平翻转次数 */
static int toggles_since(uint8_t pin, uint32_t from_tick)
{
    int n = 0, last = -1;
    for (int i = 0; i < g_wr_count; i++)
    {
        if (g_wr[i].pin != pin || g_wr[i].tick < from_tick) continue;
        if (last >= 0 && g_wr[i].level != last) n++;
        last = g_wr[i].level;
    }
    return n;
}

static void reset_log(void)
{
    g_wr_count = 0;
    memset(g_pin_level, 0xFF, sizeof(g_pin_level));
}

/*============================================================================
 * main
 *============================================================================*/
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================================\n");
    printf(" LED 图案引擎离线验证\n");
    printf(" 背景：真机上 WiFi 断线后状态机切了 RETRY_WAIT，但 GPIO5 不闪\n");
    printf("============================================================\n");

    printf("\n=== 测试 1：初始化 ===\n");
    App_LED_Init();
    check(1, "App_LED_Init 完成（看上面的 info 日志确认 6/6）");

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 2：PAT_ON 应常亮、不翻转 ===\n");
    reset_log();
    g_tick = 0;
    App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_ON);
    run_ticks(200);
    printf("  WiFi 灯最终电平=%u，翻转=%d\n",
           (unsigned)g_pin_level[PIN_WIFI], toggles_since(PIN_WIFI, 0));
    check(g_pin_level[PIN_WIFI] == 1, "PAT_ON 应让灯亮");
    check(toggles_since(PIN_WIFI, 0) == 0, "PAT_ON 不应翻转（常亮）");

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 3：常亮 → 慢闪（★ 真机现象的核心 ★）===\n");
    {
        uint32_t t0 = g_tick;
        int before = g_wr_count;

        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_SLOW_BLINK);
        printf("  Set(SLOW_BLINK) 立刻写入硬件 %d 次\n", g_wr_count - before);

        run_ticks(400);          /* 4 秒；周期 1.6 秒 → 应闪 2 次以上 */
        int tg = toggles_since(PIN_WIFI, t0);
        printf("  4 秒内翻转 %d 次\n", tg);

        check(tg > 0, "SLOW_BLINK 必须产生电平翻转（否则就是灯不闪的根因）");
        check(tg >= 2, "4 秒内（周期 1.6 秒）应至少翻转 2 次");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 4：模拟真机断线全过程 ===\n");
    {
        reset_log();
        g_tick = 0;

        /* CONNECTED */
        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_ON);
        run_ticks(100);
        uint8_t lvl_connected = g_pin_level[PIN_WIFI];

        /* CONNECTED -> RETRY_WAIT */
        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_SLOW_BLINK);
        run_ticks(100);
        uint8_t lvl_retry = g_pin_level[PIN_WIFI];

        /* RETRY_WAIT -> CONNECTING */
        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_FAST_BLINK);
        run_ticks(100);

        /* -> BACKOFF */
        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_DOUBLE_BLINK);
        run_ticks(600);

        int total = toggles_since(PIN_WIFI, 0);
        printf("  连上时电平=%u，切到 RETRY_WAIT 后电平=%u，全程翻转=%d 次\n",
               (unsigned)lvl_connected, (unsigned)lvl_retry, total);
        check(total > 10, "断线重连全过程必须有大量翻转");
        check(total > 0,  "★ 若这条失败，说明 App_LED_Set 根本没让灯动 —— 就是真机问题");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 5：PAT_OFF 应灭灯 ===\n");
    {
        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_OFF);
        run_ticks(50);
        printf("  最终电平=%u\n", (unsigned)g_pin_level[PIN_WIFI]);
        check(g_pin_level[PIN_WIFI] == 0, "PAT_OFF 应让灯灭");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 6：生命灯由独立翻转接口驱动（不走池）===\n");
    {
        reset_log();
        App_LED_HeartbeatToggle();
        App_LED_HeartbeatToggle();
        App_LED_HeartbeatToggle();
        printf("  HeartbeatToggle 写入 %d 次，最终电平=%u\n",
               g_wr_count, (unsigned)g_pin_level[PIN_HB]);
        check(g_wr_count == 3, "HeartbeatToggle 应每次写一次硬件");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== 测试 7：闪烁的末态（★ 真机 bug：闪完停在灭）===\n");
    {
        /* ---- 7a. FlashEx(..., 1)：闪三下 → 必须停在【亮】 ---- */
        reset_log();
        g_tick = 0;

        App_LED_FlashEx(APP_LED_WIFI, 3, 1);
        run_ticks(400);

        int tg_on = toggles_since(PIN_WIFI, 0);
        printf("  FlashEx(3, end_on=1): 翻转 %d 次，最终电平=%u\n",
               tg_on, (unsigned)g_pin_level[PIN_WIFI]);
        check(tg_on >= 4, "闪三下应有足够翻转（亮灭各算一次）");
        check(g_pin_level[PIN_WIFI] == 1,
              "★ 闪完必须停在【亮】—— 这正是真机上'有音频灯不亮'的根因");

        /* ---- 7b. Flash(..., 0)：闪两下 → 必须停在【灭】 ---- */
        reset_log();
        g_tick = 0;

        App_LED_Flash(APP_LED_WIFI, 2);
        run_ticks(400);

        int tg_off = toggles_since(PIN_WIFI, 0);
        printf("  Flash(2, end_on=0):   翻转 %d 次，最终电平=%u\n",
               tg_off, (unsigned)g_pin_level[PIN_WIFI]);
        check(tg_off >= 2, "闪两下应有翻转");
        check(g_pin_level[PIN_WIFI] == 0,
              "闪完必须停在【灭】（用于'没有音频'的提示）");

        /* ---- 7c. 复现真机成功分支的调用序列 ---- */
        reset_log();
        g_tick = 0;

        App_LED_Set(APP_LED_WIFI, &APP_LED_PAT_ON);
        App_LED_FlashEx(APP_LED_WIFI, 3, 1);
        run_ticks(400);
        printf("  Set(ON) 后接 FlashEx(3,1): 最终电平=%u\n",
               (unsigned)g_pin_level[PIN_WIFI]);
        check(g_pin_level[PIN_WIFI] == 1,
              "★ 模拟 task_key.c 成功分支：最终必须是亮");
    }

    printf("\n============================================================\n");
    printf(" 结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    printf("============================================================\n");
    return (g_fail == 0) ? 0 : 1;
}
