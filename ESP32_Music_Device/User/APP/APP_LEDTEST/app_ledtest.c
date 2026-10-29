/**
 * @file app_ledtest.c
 * @brief LED 自检应用层实现（全引脚扫描定位）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 三个阶段，全部只操作「映射表里已声明为输出的句柄」，不碰 flash/PSRAM 占用的引脚：
 *
 *   阶段 1  逐个点亮映射表里的输出脚（6 个指示灯 + g_gpio1/g_gpio2，共 8 个），
 *           每个 1 秒，串口同步打印 GPIO 号 → 用来核对「代码点哪个脚 / 哪个灯亮」
 *   阶段 2  全部一起亮灭 3 次 → 确认灯组整体和供电没问题
 *   阶段 3  扫描安全区里剩下的空闲引脚（定义为输出），每脚 1 秒
 *           → 找出「接在映射表之外」的灯
 *   最后    打印结论，之后交回正常 LED 逻辑
 *
 *   ★ 为什么阶段 3 要跳过一堆脚：
 *     - GPIO11~17 被 flash 和 Octal PSRAM 占用，往那些脚写电平会破坏运行中的
 *       flash/PSRAM 访问 → 直接死机
 *     - 已占用的脚（按键 / I2S / UART0 / 原生 USB）碰了会打断相应外设
 *     宁可少扫几个脚，也不能把板子写死。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "app_ledtest.h"
// 功能包含
#include "app_led.h"
#include "bsp_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if APP_LEDTEST_ENABLE
#include "esp_log.h"
#endif
/* ★ 不再 include driver/gpio.h ★
 * 原本这里直接调 gpio_config/gpio_set_level 去点亮映射表之外的引脚，
 * 那违反了分层约定（APP 层不得直接调驱动）。
 * 现在改走 BSP 层的小接口 BSP_GPIO_PulsePinTemporary()，
 * 分层恢复了，功能不变。详见 bsp_gpio.h 里该接口的说明。 */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
#if APP_LEDTEST_ENABLE
static const char *TAG = "LEDTEST";

/**
 * @brief 判断某个 GPIO 是否属于「不要扫」的集合
 * @note  返回 1 = 跳过
 */
static int app_ledtest_is_reserved(int pin)
{
    switch (pin)
    {
        case 0:                         /* BOOT 按键 / strapping */
        case 1:  case 2:                /* g_gpio1 / g_gpio2（阶段 1 已扫）；2 还是 I2S DIN */
        case 3:                         /* I2S BCLK（MAX98357） */
        case 4:  case 5:  case 6:       /* 已分配给指示灯，阶段 1 已扫 */
        case 7:  case 8:  case 9:
        case 11: case 12: case 13:      /* flash D0~D3 禁区 */
        case 14:                        /* VDD_SPI strapping 相关 */
        case 15: case 16: case 17:      /* flash/PSRAM D4,D5 + VDD_SPI strapping 禁区 */
        case 18:                        /* VDD_SPI strapping */
        case 19: case 20:               /* 原生 USB D-/D+ */
        case 21:                        /* KEY1 */
        case 38: case 39: case 40:      /* KEY2~4 */
        case 41: case 42:               /* I2S SCK / WS */
        case 43: case 44:               /* UART0 日志串口 */
        case 45:                        /* strapping */
        case 46: case 47: case 48:      /* I2S LRC / SD 及该组剩余脚 */
            return 1;
        default:
            return 0;
    }
}

/**
 * @brief 把一个引脚临时配成输出并点亮，延时后熄灭
 */
static void app_ledtest_pulse_pin(int pin, uint32_t ms)
{
    /* 走 BSP 接口：配置 → 点亮 → 保持 → 熄灭 → 释放为高阻 */
    BSP_GPIO_PulsePinTemporary(pin, GPIO_LEVEL_HIGH, ms);
}

/**
 * @brief 把测试碰过的引脚恢复成输入（高阻），避免干扰后续正常逻辑
 */
/* app_ledtest_release_pin() 已删除：
 * 释放动作现在由 BSP_GPIO_PulsePinTemporary() 内部完成，
 * 这样"配置/驱动/释放"三件事在 BSP 层是一个原子语义，调用方不会漏掉释放。 */

/**
 * @brief 把「要扫的输出句柄」收集到一张数组里
 * @param out  输出数组
 * @param cap  数组容量
 * @retval 实际收集到的句柄数
 * @note  从 App_LED_GetTable() 取 6 个指示灯句柄，再补上 g_gpio1 / g_gpio2
 *        （模板原来接灯的那两个脚，你的灯很可能就在其中）。
 *        ★ 句柄是运行期指针，所以只能运行期收集，不能做静态数组初始化。
 */
static unsigned app_ledtest_collect(const BSP_GPIO_Handle_t **out, unsigned cap)
{
    unsigned n = 0;
    const BSP_GPIO_Handle_t *const *tbl = App_LED_GetTable();

    for (int i = 0; i < APP_LED_COUNT && n < cap; i++)
    {
        if (tbl[i] != NULL && BSP_GPIO_IsOutput(tbl[i]))
            out[n++] = tbl[i];
    }

    if (n < cap && g_gpio1 != NULL) out[n++] = g_gpio1;
    if (n < cap && g_gpio2 != NULL) out[n++] = g_gpio2;

    return n;
}

/**
 * @brief 阶段 1：逐个点亮映射表里的输出句柄
 */
static void app_ledtest_stage1(void)
{
    const BSP_GPIO_Handle_t *outs[APP_LED_COUNT + 2];
    unsigned n = app_ledtest_collect(outs, (unsigned)(APP_LED_COUNT + 2));

    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, " stage 1: 逐个点亮映射表里的 %u 个输出脚", n);
    ESP_LOGI(TAG, " ★ 注意串口现在报的是哪个 GPIO 号，同时看哪个灯亮 ★");
    ESP_LOGI(TAG, "========================================================");

    for (unsigned i = 0; i < n; i++)
    {
        int pin = BSP_GPIO_GetNum(outs[i]);

        ESP_LOGW(TAG, ">>> #%u 正在点亮 GPIO%d  <<<", i + 1, pin);

        BSP_GPIO_SetLevel(outs[i], GPIO_LEVEL_HIGH);
        vTaskDelay(pdMS_TO_TICKS(APP_LEDTEST_STEP_MS));
        BSP_GPIO_SetLevel(outs[i], GPIO_LEVEL_LOW);
        vTaskDelay(pdMS_TO_TICKS(120));      /* 留一点熄灭间隔，避免看串行 */
    }

    ESP_LOGI(TAG, "stage 1 done");
}

/**
 * @brief 阶段 2：全部一起亮灭 3 次
 */
static void app_ledtest_stage2(void)
{
    const BSP_GPIO_Handle_t *outs[APP_LED_COUNT + 2];
    unsigned n = app_ledtest_collect(outs, (unsigned)(APP_LED_COUNT + 2));

    ESP_LOGI(TAG, "stage 2: 全部一起亮灭 3 次（共 %u 个脚）", n);

    for (int round = 0; round < 3; round++)
    {
        for (unsigned i = 0; i < n; i++)
            BSP_GPIO_SetLevel(outs[i], GPIO_LEVEL_HIGH);
        vTaskDelay(pdMS_TO_TICKS(400));

        for (unsigned i = 0; i < n; i++)
            BSP_GPIO_SetLevel(outs[i], GPIO_LEVEL_LOW);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    ESP_LOGI(TAG, "stage 2 done");
}

/**
 * @brief 阶段 3：扫描安全区里剩下的空闲引脚
 */
static void app_ledtest_stage3(void)
{
    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, " stage 3: 扫描 GPIO%d~GPIO%d 里的空闲脚",
             APP_LEDTEST_SWEEP_FROM, APP_LEDTEST_SWEEP_TO);
    ESP_LOGI(TAG, " 若某个脚上挂的灯亮了，说明它没接在映射表假设的位置");
    ESP_LOGI(TAG, "========================================================");

    for (int pin = APP_LEDTEST_SWEEP_FROM; pin <= APP_LEDTEST_SWEEP_TO; pin++)
    {
        if (app_ledtest_is_reserved(pin))
            continue;

        ESP_LOGW(TAG, ">>> 扫描 GPIO%d  <<<", pin);
        app_ledtest_pulse_pin(pin, APP_LEDTEST_STEP_MS);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_LOGI(TAG, "stage 3 done");
}
#endif /* APP_LEDTEST_ENABLE —— 以下任务函数在两个分支下都要存在（main.c 会引用它） */

/*==============================================================================
 * 任务函数
 *============================================================================*/
void Task_LED_Test(void *argument)
{
    (void)argument;

#if !APP_LEDTEST_ENABLE
    /* 自检关闭：什么都不做，任务立即退出（不占 RAM、不产生编译警告） */
    vTaskDelete(NULL);
#else
    /* 等 LED 服务任务把句柄和图案初始化好 */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGW(TAG, "############ LED 自检启动（APP_LEDTEST_ENABLE = 1）############");
    ESP_LOGW(TAG, "看完后请把 User/APP/APP_LEDTEST/app_ledtest.h 里的开关改回 0");

    app_ledtest_stage1();
    app_ledtest_stage2();
    app_ledtest_stage3();

    ESP_LOGW(TAG, "############ 自检结束，交回正常 LED 逻辑 ############");
    ESP_LOGW(TAG, "结论怎么看：");
    ESP_LOGW(TAG, "  1) 阶段1/2 里有灯亮 -> 灯接在映射表内的脚上，");
    ESP_LOGW(TAG, "     记下亮的那个 GPIO 号，改 app_led_cfg.h / bsp_gpio.c 的 GPIO_HW_MAP");
    ESP_LOGW(TAG, "  2) 阶段3 才有灯亮   -> 灯接在映射表之外的脚上，");
    ESP_LOGW(TAG, "     告诉我那个 GPIO 号，我把它加进 GPIO_HW_MAP");
    ESP_LOGW(TAG, "  3) 三个阶段都没灯亮 -> 不是引脚问题，是硬件：");
    ESP_LOGW(TAG, "     查 LED 极性是否装反、限流电阻是否漏焊、共地是否接好");
    ESP_LOGW(TAG, "  4) 如果灯是「常亮」而不是跟着闪 -> LED 是低电平点亮接法（接 3V3），");
    ESP_LOGW(TAG, "     需要在 bsp_gpio.c 里把电平反过来，告诉我即可");

    vTaskDelete(NULL);
#endif
}
