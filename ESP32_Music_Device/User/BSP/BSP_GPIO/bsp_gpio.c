/**
 * @file bsp_gpio.c
 * @brief GPIO 通用 BSP 层实现（内置硬件映射表 + 具名句柄）
 * @author 李嘉图
 * @date 2026-06-17
 *
 * @note 映射表把引脚配置写死在本文件，同时兼作句柄表：具名句柄直接指向表项，
 *       上层拿去就用，不必查表，也没有逻辑编号。
 *
 *==============================================================================
 * ★ 本工程的点位分配（5 个语义指示灯 + 4 个按键）★
 *==============================================================================
 *   句柄          引脚      语义
 *   -----------   -------   ------------------------------------------------
 *   g_led_hb      GPIO4     生命灯，500ms 方波（由独立任务翻转，见 app_led.c）
 *   g_led_wifi    GPIO5     WiFi 状态：常亮=已连上并拿到 IP
 *   g_led_mic     GPIO6     正在录音
 *   g_led_audio   GPIO7     有可播放音频；载入新音频时快闪三下
 *   g_led_backend GPIO8     后端连通：常亮=上次请求成功
 *   g_led_spare   GPIO9     预留槽位（本工程未使用）
 *   g_key1~4      GPIO21/38/39/40   低电平按下，内部上拉
 *   g_gpio1/2     GPIO1/GPIO2       通用双向脚（模板自带的那两个）
 *
 *   指示灯语义改动请改 User/APP/APP_LED/app_led_cfg.h；
 *   引脚改动只改本文件的 GPIO_HW_MAP。
 *
 *==============================================================================
 * ★ ESP32-S3-N16R8 引脚禁区（换板/加外设前必读）★
 *==============================================================================
 *  本模块配置的是 ESP32-S3-WROOM-1-N16R8（16MB flash + 8MB Octal PSRAM）。
 *  Octal PSRAM 与 flash 吃掉了大量 GPIO，下列引脚【绝对不能】再分配给外设，
 *  否则表现为：上电反复重启 / PSRAM 初始化失败 / flash 读写异常（很难排查）：
 *
 *    引脚          用途                                  是否可用
 *    -----------   -----------------------------------   --------
 *    GPIO11~14     flash D0~D3 (SPI0/1)                  ❌ 禁止
 *    GPIO16,17     flash/PSRAM D4,D5 (Octal)             ❌ 禁止
 *    GPIO26        PSRAM CS                                ❌ 禁止
 *    GPIO27~32     PSRAM DQ1~DQ6 / DQS / CLK（Octal）      ❌ 禁止
 *    GPIO33~37     PSRAM DQ7 及 Octal 扩展线               ❌ 禁止
 *    GPIO43,44     UART0 TXD/RXD（串口日志，CONSOLE UART0）📌 保留
 *    GPIO19,20     原生 USB D-/D+（USB 烧录/CDC）          ⚠️ 慎用
 *    GPIO0,3,45,46 strapping（上电电平决定启动模式）       ⚠️ 见下方说明
 *    GPIO15~18     VDD_SPI 相关 strapping（上电须为高）    ⚠️ 见下方说明
 *
 *  本工程把所有 LED/KEY 都放在 GPIO1~9、21、38~40 上，全部落在安全区；
 *  音频占 GPIO2/3/41/42/46/47/48（见 bsp_i2s.c）。
 *
 *  strapping 注意事项：
 *    - GPIO0  : 上电低 = 进下载模式。板载 BOOT 键，别当普通外设用。
 *    - GPIO45  : 上电高 = VDD_SPI 由 3.3V 供电（N16R8 必须为高，内部默认拉高）。
 *    - GPIO46  : 上电最好为低。本工程当 I2S TX 用，复位瞬间为高阻 + 内部下拉，
 *                正好满足；且 sdkconfig 里 CONFIG_BOOT_ROM_LOG_ALWAYS_ON=y，
 *                ROM 日志被强制打开，不再受该脚 strapping 影响。
 *    - GPIO3   : 上电电平决定 JTAG 信号源（内建 USB-JTAG / 引脚 JTAG）。悬空即可，
 *                本工程当 I2S TX 输出用，复位瞬间引脚为高阻，不影响。
 *    - GPIO15~18: VDD_SPI 电压选择，上电必须为高。本工程【没有】把 LED 接在这些脚上，
 *                就是为了避开这个坑（早期版本曾把 LED9~12 放这里，已挪走）。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include "ds_common.h"      /* ARRAY_SIZE + bit_onehot_index（两个旧头已合并） */
#include "bsp_gpio.h"
// 功能包含
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 内部宏：映射表可读性辅助
 *============================================================================*/
/** @brief 引脚掩码（one-hot，ESP-IDF 风格） */
#define PIN_MASK(n)     (1ULL << (n))

/* LED 脚：双向（INPUT_OUTPUT）。
 * 两个原因：
 *   1. ESP-IDF 的 gpio_get_level() 读的是输入通道，纯输出模式下读回恒为 0，
 *      会让"读回再取反"的 Toggle 退化成"每次都写高"（LED 表现为常亮不闪）
 *   2. 双向脚可以读回真实电平，方便自检时确认写进去了没有
 * 上电状态：内部上下拉全关，引脚高阻 → LED 不亮（高电平点亮的接法下这是确定的灭） */
#define LED_CFG(n)                                              \
    {                                                           \
        .pin_bit_mask = PIN_MASK(n),                            \
        .mode         = GPIO_MODE_INPUT_OUTPUT,                 \
        .pull_up_en   = GPIO_PULLUP_DISABLE,                    \
        .pull_down_en = GPIO_PULLDOWN_DISABLE,                  \
        .intr_type    = GPIO_INTR_DISABLE,                      \
    }

/* KEY 脚：纯输入 + 内部上拉。按键另一端接 GND，按下读到 0。
 * ★ 必须使能内部上拉，否则按键没按下时引脚悬空，读数随机抖动。 */
#define KEY_CFG(n)                                              \
    {                                                           \
        .pin_bit_mask = PIN_MASK(n),                            \
        .mode         = GPIO_MODE_INPUT,                        \
        .pull_up_en   = GPIO_PULLUP_ENABLE,                     \
        .pull_down_en = GPIO_PULLDOWN_DISABLE,                  \
        .intr_type    = GPIO_INTR_DISABLE,                      \
    }

/*==============================================================================
 * 类型定义与硬件映射表
 *============================================================================*/
/**
 * @brief GPIO 句柄结构体定义（对上层不透明）
 */
struct BSP_GPIO_Handle {
    gpio_config_t gpio_init_struct;     // 引脚配置
};

/**
 * @brief 硬件映射表：引脚及其配置
 * @note  pin_bit_mask 用 ESP-IDF 的位掩码写法（1ULL << 引脚号），物理引脚号不另存一份，
 *        运行时由 bit_onehot_index() 还原，避免两处写不一致。
 *
 *        方向由 mode 决定，gpio_mode_t 是位标志，可组合：
 *          GPIO_MODE_INPUT        读        GPIO_MODE_OUTPUT        写
 *          GPIO_MODE_INPUT_OUTPUT 读+写     GPIO_MODE_OUTPUT_OD     开漏写
 *        ★ 判断方向要用位与，不能用 == ★ 否则 GPIO_MODE_OUTPUT_OD 这类组合模式会被误判。
 *
 *        ★★ 新增句柄时必须同时加进本表，且在 bsp_gpio.h 里 extern 出来 ★★
 *        否则 BSP_GPIO_Init() 不会配置该引脚，表现为「代码里控制了但硬件不动」。
 */
static const struct BSP_GPIO_Handle GPIO_HW_MAP[] = {
    /* ---- 索引 0~5：指示灯，高电平点亮 ---- */
    { .gpio_init_struct = LED_CFG(4) },     /* [0] g_led_hb      生命灯 500ms 方波 */
    { .gpio_init_struct = LED_CFG(5) },     /* [1] g_led_wifi    WiFi 状态 */
    { .gpio_init_struct = LED_CFG(6) },     /* [2] g_led_mic     正在录音 */
    { .gpio_init_struct = LED_CFG(7) },     /* [3] g_led_audio   有可播放音频 */
    { .gpio_init_struct = LED_CFG(8) },     /* [4] g_led_backend 后端连通 */
    { .gpio_init_struct = LED_CFG(9) },     /* [5] g_led_spare   预留 */

    /* ---- 索引 6~9：按键，低电平按下，内部上拉 ---- */
    { .gpio_init_struct = KEY_CFG(21) },    /* [6]  g_key1  按住录音 */
    { .gpio_init_struct = KEY_CFG(38) },    /* [7]  g_key2  播放合成音频 */
    { .gpio_init_struct = KEY_CFG(39) },    /* [8]  g_key3  回环录音 */
    { .gpio_init_struct = KEY_CFG(40) },    /* [9]  g_key4  播放回环录音 */

    /* ---- 索引 10~11：通用备用（兼容模板旧句柄名） ---- */
    { .gpio_init_struct = LED_CFG(1) },     /* [10] g_gpio1  通用双向 */
    { .gpio_init_struct = LED_CFG(2) },     /* [11] g_gpio2  通用双向 */
};

/*==============================================================================
 * 内部：映射表的分段边界
 *============================================================================*/
/**
 * @brief 映射表分段：把「指示灯」和「按键」切成两段，便于各自初始化
 * @note  ★ 为什么需要这个 ★
 *        最初的写法是只有一个 BSP_GPIO_Init() 遍历整张表。这在只有一个任务用 GPIO 时没问题，
 *        但本工程 LED 服务任务和按键扫描任务是两个独立任务，各自调一次 Init()，
 *        按键引脚就会被 gpio_config() 两次。
 *        虽然重复配置同样的参数结果一样（无害），但语义上不清晰，而且以后
 *        万一某段配置改成依赖运行期状态，重复配置就会出问题。
 *        所以明确拆成 InitOutputs / InitInputs，各自只碰自己那一段。
 */
#define GPIO_MAP_LED_FIRST      0
#define GPIO_MAP_LED_COUNT      6       /* 索引 0~5：6 个指示灯 */
#define GPIO_MAP_KEY_FIRST      6
#define GPIO_MAP_KEY_COUNT      4       /* 索引 6~9：4 个按键 */

/*==============================================================================
 * 具名句柄
 *============================================================================*/
/**
 * @brief 具名句柄，指向映射表中的表项
 * @note  ★ 新增句柄时必须同时加进 GPIO_HW_MAP ★
 *        指向表项而非副本，是为了让 Init() 能遍历同一张表，配置也只有一份。
 */
/* 指示灯 */
const BSP_GPIO_Handle_t *const g_led_hb      = &GPIO_HW_MAP[0];
const BSP_GPIO_Handle_t *const g_led_wifi    = &GPIO_HW_MAP[1];
const BSP_GPIO_Handle_t *const g_led_mic     = &GPIO_HW_MAP[2];
const BSP_GPIO_Handle_t *const g_led_audio   = &GPIO_HW_MAP[3];
const BSP_GPIO_Handle_t *const g_led_backend = &GPIO_HW_MAP[4];
const BSP_GPIO_Handle_t *const g_led_spare   = &GPIO_HW_MAP[5];

/* 按键 */
const BSP_GPIO_Handle_t *const g_key1        = &GPIO_HW_MAP[6];
const BSP_GPIO_Handle_t *const g_key2        = &GPIO_HW_MAP[7];
const BSP_GPIO_Handle_t *const g_key3        = &GPIO_HW_MAP[8];
const BSP_GPIO_Handle_t *const g_key4        = &GPIO_HW_MAP[9];

/* 通用 */
const BSP_GPIO_Handle_t *const g_gpio1       = &GPIO_HW_MAP[10];
const BSP_GPIO_Handle_t *const g_gpio2       = &GPIO_HW_MAP[11];

/*==============================================================================
 * 初始化函数
 *============================================================================*/
/**
 * @brief 逐条下发映射表某一段的引脚配置
 * @param first 起始索引
 * @param count 条目数
 * @note  不检查返回值：单个引脚配置失败不中断其余的。
 */
static void bsp_gpio_config_range(size_t first, size_t count)
{
    for (size_t i = first; i < first + count && i < ARRAY_SIZE(GPIO_HW_MAP); i++)
    {
        gpio_config(&GPIO_HW_MAP[i].gpio_init_struct);
    }
}

/**
 * @brief GPIO BSP 层初始化（整张表：指示灯 + 按键 + 通用）
 */
void BSP_GPIO_Init(void)
{
    bsp_gpio_config_range(0, ARRAY_SIZE(GPIO_HW_MAP));
}

/**
 * @brief 只配置指示灯
 */
void BSP_GPIO_InitOutputs(void)
{
    bsp_gpio_config_range(GPIO_MAP_LED_FIRST, GPIO_MAP_LED_COUNT);
}

/**
 * @brief 只配置按键输入
 */
void BSP_GPIO_InitInputs(void)
{
    bsp_gpio_config_range(GPIO_MAP_KEY_FIRST, GPIO_MAP_KEY_COUNT);
}

/*==============================================================================
 * 数字输入
 *============================================================================*/
/**
 * @brief 读取引脚电平
 * @param handle 句柄
 * @retval 1: 高电平
 * @retval 0: 低电平
 * @retval 0xFF: 失败（句柄为 NULL，或该脚不含输入位，或引脚号还原失败）
 * @note 只允许读含输入位的脚，纯输出脚输入通道是关的，读也读不到。
 */
uint8_t BSP_GPIO_ReadLevel(const BSP_GPIO_Handle_t *handle)
{
    if (handle == NULL)
        return 0xFF;

    /* 只允许读含输入位 BIT(0) 的脚 */
    if ((handle->gpio_init_struct.mode & GPIO_MODE_INPUT) == 0)
        return 0xFF;

    uint8_t gpio_num = bit_onehot_index(handle->gpio_init_struct.pin_bit_mask);
    if (gpio_num == 0xFF)
        return 0xFF;

    return (uint8_t)gpio_get_level((gpio_num_t)gpio_num);
}

/*==============================================================================
 * 数字输出
 *============================================================================*/
/**
 * @brief 设置引脚电平
 * @param handle 句柄
 * @param level  电平：GPIO_LEVEL_HIGH 或 GPIO_LEVEL_LOW
 * @retval 1: 成功
 * @retval 0: 失败（句柄为 NULL，或该脚不含输出位，或引脚号还原失败）
 * @note  非 0 的 level 一律当高电平处理，与 STM32 HAL 一致。
 *        只允许写含输出位的脚，纯输入脚直接返回 0，不碰硬件。
 *        开漏脚写 1 是释放总线而非驱动高，实际电平由上拉决定。
 */
uint8_t BSP_GPIO_SetLevel(const BSP_GPIO_Handle_t *handle, BSP_GPIO_Level_t level)
{
    if (handle == NULL)
        return 0;

    /* 只允许写含输出位 BIT(1) 的脚 */
    if ((handle->gpio_init_struct.mode & GPIO_MODE_OUTPUT) == 0)
        return 0;

    uint8_t gpio_num = bit_onehot_index(handle->gpio_init_struct.pin_bit_mask);
    if (gpio_num == 0xFF)
        return 0;

    return (gpio_set_level((gpio_num_t)gpio_num, level ? 1 : 0) == ESP_OK) ? 1 : 0;
}

/**
 * @brief 翻转引脚电平
 * @param handle 句柄
 * @retval 1: 成功
 * @retval 0: 失败（句柄为 NULL，或该脚不是双向模式）
 * @note  只能翻转双向脚（INPUT_OUTPUT）。ESP-IDF 没有 HAL_GPIO_TogglePin 的对应函数，
 *        只能读回当前电平再取反写回，所以输入位和输出位缺一不可。
 */
uint8_t BSP_GPIO_Toggle(const BSP_GPIO_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    /* 翻转＝读回再写反，输入位 BIT(0) 和输出位 BIT(1) 缺一不可 */
    if ((handle->gpio_init_struct.mode & GPIO_MODE_INPUT) == 0 ||
        (handle->gpio_init_struct.mode & GPIO_MODE_OUTPUT) == 0)
        return 0;

    uint8_t gpio_num = bit_onehot_index(handle->gpio_init_struct.pin_bit_mask);
    if (gpio_num == 0xFF)
        return 0;

    /* 读到的是引脚真实电平（走输入通道，不是输出寄存器），取反写回 */
    int level = gpio_get_level((gpio_num_t)gpio_num);
    return (gpio_set_level((gpio_num_t)gpio_num, !level) == ESP_OK) ? 1 : 0;
}

/*==============================================================================
 * 句柄自省
 *============================================================================*/
/**
 * @brief 查询句柄对应的物理引脚号
 * @param handle 句柄
 * @retval 0~48: 物理引脚号
 * @retval 0xFF : 失败
 */
uint8_t BSP_GPIO_GetNum(const BSP_GPIO_Handle_t *handle)
{
    if (handle == NULL)
        return 0xFF;

    return bit_onehot_index(handle->gpio_init_struct.pin_bit_mask);
}

/**
 * @brief 查询引脚是否具备输出能力
 * @param handle 句柄
 * @retval 1: 可写
 * @retval 0: 不可写
 */
uint8_t BSP_GPIO_IsOutput(const BSP_GPIO_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    return (handle->gpio_init_struct.mode & GPIO_MODE_OUTPUT) ? 1 : 0;
}

/**
 * @brief 查询引脚是否具备输入能力
 * @param handle 句柄
 * @retval 1: 可读
 * @retval 0: 不可读
 */
uint8_t BSP_GPIO_IsInput(const BSP_GPIO_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    return (handle->gpio_init_struct.mode & GPIO_MODE_INPUT) ? 1 : 0;
}

/*==============================================================================
 * 临时引脚测试（仅供诊断工具）
 *============================================================================*/
/**
 * @brief 把任意引脚临时配成输出、驱动一段时间、再释放为高阻输入
 * @note  ★ 这是 BSP 层唯一一个"不经过映射表"的接口 ★
 *        存在的唯一原因是诊断工具需要扫描映射表之外的引脚。
 *        所以它刻意保持最小：配置 → 驱动 → 保持 → 拉低 → 释放。
 *
 *        ★ 用 vTaskDelay 而不是忙等 ★
 *          调用方是任务上下文（APP_LEDTEST 在任务里跑），
 *          忙等几百毫秒会白烧 CPU 并可能让看门狗报警。
 *          本函数内部依赖 FreeRTOS，但**只在 .c 里**依赖，
 *          头文件仍然是纯 C（不泄漏 IDF 类型）——分层约定没有被破坏。
 */
int BSP_GPIO_PulsePinTemporary(int pin, BSP_GPIO_Level_t level, uint32_t hold_ms)
{
    /* ESP32-S3 只有 GPIO0~48，越界的直接拒绝
     * （注意：这里只做范围检查，禁区检查是调用方的板级责任，见头文件说明） */
    if (pin < 0 || pin > 48)
        return -1;

    gpio_config_t cfg = {
        .pin_bit_mask = PIN_MASK(pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT,   /* 和映射表里的 LED 保持一致，可读回 */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&cfg) != ESP_OK)
        return -1;

    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);

    if (hold_ms > 0)
        vTaskDelay(pdMS_TO_TICKS(hold_ms));

    gpio_set_level((gpio_num_t)pin, 0);     /* 熄灭：高电平点亮，写低 = 灭 */

    /* 释放为高阻输入，避免这个脚在自检结束后还保持着输出驱动
     * （比如它恰好是某个外设的输入脚，被我们驱动着就不对了） */
    gpio_config_t release = {
        .pin_bit_mask = PIN_MASK(pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&release);

    return 0;
}