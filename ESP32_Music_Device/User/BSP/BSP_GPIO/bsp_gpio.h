/**
 * @file bsp_gpio.h
 * @brief GPIO 通用 BSP 层（句柄式接口）
 * @author 李嘉图
 * @date 2026-06-17
 *
 * @note 上层用 BSP 暴露的具名句柄操作引脚，不传引脚号：
 *
 *           BSP_GPIO_SetLevel(g_led_hb, GPIO_LEVEL_HIGH);
 *
 *       句柄的引脚配置在 bsp_gpio.c 的映射表里写死，换板子只改那张表。
 *       句柄结构体对上层不透明，因此本头文件不依赖 ESP-IDF。
 *
 *       ★ 本工程的 5 个 LED 有明确语义，指示灯位与闪烁图案见 app_led_cfg.h ★
 *       ★ ESP32-S3-N16R8 引脚禁区见 bsp_gpio.c 顶部的说明 ★
 */

#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
#define GPIO_MAX_NUM    32    /* 最大支持 GPIO 数量（ESP32-S3 共 48 个，这里留足余量） */

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief GPIO 句柄（不透明类型，成员定义在 bsp_gpio.c）
 */
typedef struct BSP_GPIO_Handle BSP_GPIO_Handle_t;

/**
 * @brief GPIO 电平
 */
typedef enum {
    GPIO_LEVEL_LOW  = 0,    // 低电平
    GPIO_LEVEL_HIGH = 1,    // 高电平
} BSP_GPIO_Level_t;

/*==============================================================================
 * 具名句柄：指示灯（高电平点亮，共 5 个语义灯 + 1 个预留）
 *============================================================================*/
/**
 * @brief 具名句柄：引脚已写死在映射表里，上层直接拿去用
 * @note  用法同 STM32 的 extern UART_HandleTypeDef huart1，
 *        但指针本身也是 const，上层改不了它指向哪个引脚。
 *
 *        LED 是高电平点亮：LED_ON  = BSP_GPIO_SetLevel(h, GPIO_LEVEL_HIGH)
 *                        LED_OFF = BSP_GPIO_SetLevel(h, GPIO_LEVEL_LOW)
 *
 *        ★ 语义与闪烁图案集中在 User/APP/APP_LED/app_led_cfg.h，改语义改那里 ★
 */
extern const BSP_GPIO_Handle_t *const g_led_hb;        /* LED1 生命灯：500ms 方波，说明系统还活着 */
extern const BSP_GPIO_Handle_t *const g_led_wifi;      /* LED2 WiFi 状态：常亮 = 已连上并拿到 IP */
extern const BSP_GPIO_Handle_t *const g_led_mic;       /* LED3 正在录音 */
extern const BSP_GPIO_Handle_t *const g_led_audio;     /* LED4 是否有可播放音频；有新音频时快闪三下 */
extern const BSP_GPIO_Handle_t *const g_led_backend;   /* LED5 后端连通：常亮 = 上次请求成功 */
extern const BSP_GPIO_Handle_t *const g_led_spare;     /* LED6 预留（映射表留了槽位，本工程未用） */

/*==============================================================================
 * 具名句柄：KEY（低电平按下，共 4 个）
 *============================================================================*/
extern const BSP_GPIO_Handle_t *const g_key1;          /* KEY1 按住录音 / 松手停止并上传 */
extern const BSP_GPIO_Handle_t *const g_key2;          /* KEY2 播放合成音频（循环）/ 再按停止 */
extern const BSP_GPIO_Handle_t *const g_key3;          /* KEY3 回环测试：按一下录固定 5 秒存本地 */
extern const BSP_GPIO_Handle_t *const g_key4;          /* KEY4 播放回环录音（单次）/ 再按停止 */

/*==============================================================================
 * 具名句柄：通用备用
 *============================================================================*/
extern const BSP_GPIO_Handle_t *const g_gpio1;         /* 通用双向 IO（兼容模板旧名） */
extern const BSP_GPIO_Handle_t *const g_gpio2;         /* 通用双向 IO（兼容模板旧名） */

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief GPIO BSP 层初始化
 * @note  把映射表里的引脚配置下发给驱动，启动时调用一次。
 *        不检查单条配置的返回值：单个引脚配置失败不中断其余的。
 *        重复调用是幂等的。
 */
void BSP_GPIO_Init(void);

/**
 * @brief 只配置映射表里的指示灯（LED），不碰按键
 * @note  ★ 存在的意义：按键和 LED 共用同一张映射表，如果 LED 任务和按键任务
 *          各自调 BSP_GPIO_Init()，按键引脚会被配置两次。
 *          分成两个函数后，各层只初始化自己关心的那部分，语义清晰、也没有交叉影响。
 *        ★ 两个函数都必须在第一次读写引脚之前调用。
 */
void BSP_GPIO_InitOutputs(void);

/**
 * @brief 只配置映射表里的按键输入，不碰指示灯
 */
void BSP_GPIO_InitInputs(void);

/*==============================================================================
 * 数字输入
 *============================================================================*/
/**
 * @brief 读取引脚电平
 * @param handle 句柄
 * @retval 1: 高电平
 * @retval 0: 低电平
 * @retval 0xFF: 失败（句柄为 NULL，或该脚不含输入位）
 * @note  纯输出脚不能读，返回 0xFF；双向脚返回的是引脚真实电平。
 */
uint8_t BSP_GPIO_ReadLevel(const BSP_GPIO_Handle_t *handle);

/*==============================================================================
 * 数字输出
 *============================================================================*/
/**
 * @brief 设置引脚电平
 * @param handle 句柄
 * @param level  电平：GPIO_LEVEL_HIGH 或 GPIO_LEVEL_LOW
 * @retval 1: 成功
 * @retval 0: 失败（句柄为 NULL，或该脚不含输出位）
 */
uint8_t BSP_GPIO_SetLevel(const BSP_GPIO_Handle_t *handle, BSP_GPIO_Level_t level);

/**
 * @brief 翻转引脚电平
 * @param handle 句柄
 * @retval 1: 成功
 * @retval 0: 失败（句柄为 NULL，或该脚不是双向模式）
 * @note  只能翻转双向脚（映射表里配成 GPIO_MODE_INPUT_OUTPUT）。
 */
uint8_t BSP_GPIO_Toggle(const BSP_GPIO_Handle_t *handle);

/*==============================================================================
 * 句柄自省
 *============================================================================*/
/**
 * @brief 查询句柄对应的物理引脚号
 * @param handle 句柄
 * @retval 0~48: 物理引脚号
 * @retval 0xFF : 失败（句柄为 NULL，或掩码不是 one-hot）
 * @note  仅供日志、自检使用；业务层不应依赖引脚号，一律用句柄。
 */
uint8_t BSP_GPIO_GetNum(const BSP_GPIO_Handle_t *handle);

/**
 * @brief 查询引脚是否具备输出能力
 * @param handle 句柄
 * @retval 1: 可写
 * @retval 0: 不可写（句柄为 NULL 或纯输入脚）
 */
uint8_t BSP_GPIO_IsOutput(const BSP_GPIO_Handle_t *handle);

/**
 * @brief 查询引脚是否具备输入能力
 * @param handle 句柄
 * @retval 1: 可读
 * @retval 0: 不可读（句柄为 NULL 或纯输出脚）
 */
uint8_t BSP_GPIO_IsInput(const BSP_GPIO_Handle_t *handle);

/*==============================================================================
 * 临时引脚测试（★ 仅供诊断工具使用 ★）
 *============================================================================*/
/**
 * @brief 把任意引脚临时配成输出并驱动一段时间，然后释放为高阻输入
 * @param pin        物理引脚号
 * @param level      输出电平（GPIO_LEVEL_HIGH / GPIO_LEVEL_LOW）
 * @param hold_ms    保持多久（毫秒）
 * @retval 0: 成功
 * @retval -1: 引脚号非法（> 48）
 *
 * @note  ★ 这个接口存在的原因 ★
 *        诊断工具（APP_LEDTEST 的引脚扫描）需要点亮**不在映射表里**的引脚，
 *        以定位"灯到底焊在哪个脚上"。它原本直接在 APP 层 include 了
 *        `driver/gpio.h` —— 那违反了本工程的分层约定（APP 层不得直接调驱动）。
 *        所以把这点能力收进 BSP 层，用一个小接口换回分层的一致性。
 *
 *        ★ 只在 APP_LEDTEST_ENABLE = 1 时才会被真正调用，不影响正式固件 ★
 *          （默认关闭时链接器会把这个函数连同 gpio_config 一起丢掉）
 *
 *        ⚠️ 调用方必须自己保证引脚安全 ⚠️
 *          本函数不做任何禁区检查，因为"哪些脚能用"是板级知识、
 *          属于调用方的责任。ESP32-S3-N16R8 上**绝对不能**碰
 *          GPIO11~17、GPIO26~37（flash / Octal PSRAM），
 *          碰了会直接破坏运行中的代码执行 → 死机。
 *          本工程的禁区清单写在 bsp_gpio.c 顶部，APP_LEDTEST 里有对应的跳过表。
 */
int BSP_GPIO_PulsePinTemporary(int pin, BSP_GPIO_Level_t level, uint32_t hold_ms);

#endif /* __BSP_GPIO_H__ */
