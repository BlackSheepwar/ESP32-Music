/**
 * @file app_key.h
 * @brief 按键应用层（句柄式接口 + 内置消抖状态机 + 事件队列）
 * @author 李嘉图
 * @date 2026-06-18
 *
 * @note GPIO 引脚映射已下沉到 BSP 层（bsp_gpio.c 的 GPIO_HW_MAP），
 *       本层只认 BSP_GPIO 暴露的具名句柄（g_key1 ~ g_key4），不传引脚号、不传 gpio_id。
 *
 *       内部状态机流转：
 *         IDLE → (按下消抖稳定) → DOWN 事件 → PRESSED
 *         PRESSED → (短按释放)  → CLICK 事件 → IDLE
 *         PRESSED → (长按超时)  → LONG 事件 → LONG_HELD
 *         LONG_HELD → (释放)    → LONG_UP 事件 → IDLE
 *
 *       ★ 与参考工程 app_key.c 的差异（移植时踩过的坑）：
 *         参考工程用 gpio_id（数字）寻址 BSP，BSP_GPIO_ReadLevel(gpio_id) 把
 *         uint8_t 当指针传 —— 那是 STM32 版的写法。本工程严格用句柄指针，
 *         所以 App_Key_Register() 的参数从 gpio_id 换成了 BSP_GPIO_Handle_t*。
 *
 *       ★ 事件通过 FreeRTOS 队列抛给 TASK 层，扫描任务和业务任务解耦：
 *         扫描任务只管扫描，业务任务阻塞在队列上，按键响应不会被扫描周期拖慢。
 */

#ifndef __APP_KEY_H__
#define __APP_KEY_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
// 功能包含
#include "bsp_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*==============================================================================
 * 常量定义
 *============================================================================*/
#define KEY_MAX_NUM     8       /**< 最大支持的按键数（独立于 GPIO_MAX_NUM） */

/*==============================================================================
 * 事件枚举
 *============================================================================*/
/** @brief 按键事件类型 */
typedef enum {
    APP_KEY_EVENT_NONE      = 0,   /**< 无事件 */
    APP_KEY_EVENT_DOWN      = 1,   /**< 刚按下 */
    APP_KEY_EVENT_CLICK     = 2,   /**< 短按释放 */
    APP_KEY_EVENT_LONG      = 3,   /**< 长按触发（只触发一次） */
    APP_KEY_EVENT_LONG_UP   = 4,   /**< 长按释放 */
} App_Key_Event_t;

/** @brief 按键事件消息结构（用于消息队列） */
typedef struct {
    uint8_t         key_id;  /**< 逻辑按键 ID */
    App_Key_Event_t event;   /**< 按键事件类型 */
} App_Key_EventPacket_t;

/*==============================================================================
 * 初始化函数
 *============================================================================*/
/**
 * @brief 按键模块初始化
 * @note  内部调用 BSP_GPIO_Init() 配置所有引脚，创建事件队列，清空按键资源池。
 *        必须在 App_Key_Register() 之前调用。
 */
void App_Key_Init(void);

/**
 * @brief 注册一个按键到静态资源池
 * @param key_id         逻辑按键 ID（分发表匹配用，与硬件引脚无关）
 * @param gpio           BSP_GPIO 具名句柄（g_key1 ~ g_key4）
 * @param long_press_ms  长按判定阈值（毫秒）
 * @param active_level   按键激活电平（0 = 低电平按下，1 = 高电平按下）
 * @param debounce_ms    消抖稳定时长（毫秒，推荐 20）
 * @retval 0: 参数错误（句柄为 NULL / 不是输入脚 / key_id 重复）
 * @retval 1: 成功
 * @retval 2: 资源池已满
 * @note  key_id 和句柄分离设计：逻辑标识与硬件映射解耦，
 *        换板子只改 TASK 层的注册表里的句柄，业务流程的 key_id 不用动。
 */
uint8_t App_Key_Register(uint8_t key_id,
                         const BSP_GPIO_Handle_t *gpio,
                         uint16_t long_press_ms,
                         uint8_t active_level,
                         uint16_t debounce_ms);

/*==============================================================================
 * 更新按键状态
 *============================================================================*/
/**
 * @brief 更新所有已注册按键的状态（周期调用）
 * @param elapsed_ms 距上次调用的毫秒数（调用方传实际时间差，
 *                   这样即使任务被挤、周期抖动，长按/消抖计时也准确）
 * @note  内部遍历按键池 → 读 GPIO → 消抖状态机 → 事件入队（队列满则丢弃并计数）。
 *        不阻塞、不动态分配内存。
 */
void App_Key_Update(uint32_t elapsed_ms);

/*==============================================================================
 * 事件队列
 *============================================================================*/
/**
 * @brief 获取按键事件队列句柄
 * @retval 非 NULL: 队列（供 TASK 层 App_Key_CMD 任务阻塞等待）
 * @retval NULL: 尚未 Init
 */
QueueHandle_t App_Key_GetQueue(void);

/**
 * @brief 阻塞读取一个按键事件
 * @param msg           输出事件
 * @param timeout_ms    超时（毫秒）；0xFFFFFFFF = 永久等待
 * @retval 1: 读到事件
 * @retval 0: 超时
 * @note  对 TASK 层更友好的封装，省得它去 include FreeRTOS 的队列 API。
 */
uint8_t App_Key_WaitEvent(App_Key_EventPacket_t *msg, uint32_t timeout_ms);

/**
 * @brief 查询因队列满而丢弃的事件数（诊断用）
 */
uint32_t App_Key_GetDropCount(void);

#endif /* __APP_KEY_H__ */
