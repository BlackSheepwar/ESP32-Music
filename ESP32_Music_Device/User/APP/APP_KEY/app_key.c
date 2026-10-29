/**
 * @file app_key.c
 * @brief 按键应用层实现（句柄式接口 + 内置消抖状态机 + 事件队列）
 * @author 李嘉图
 * @date 2026-06-18
 *
 * @note GPIO 引脚映射已下沉到 BSP 层（bsp_gpio.c 的 GPIO_HW_MAP），
 *       本层通过 BSP_GPIO 具名句柄读电平，不碰引脚号。
 *
 *       内部状态机：
 *         APP_KEY_STATE_NO   → (按下消抖稳定) → APP_KEY_EVENT_DOWN    → APP_KEY_STATE_YES
 *         APP_KEY_STATE_YES  → (释放消抖稳定) → APP_KEY_EVENT_CLICK   → APP_KEY_STATE_NO
 *         APP_KEY_STATE_YES  → (长按超时)     → APP_KEY_EVENT_LONG    → APP_KEY_STATE_LONG
 *         APP_KEY_STATE_LONG → (释放)         → APP_KEY_EVENT_LONG_UP → APP_KEY_STATE_NO
 *
 *       ★ 计时全部用「毫秒累加」而不是「任务周期数」：
 *         参考工程用 counter++ 按周期计数，一旦扫描任务被高优先级任务挤掉，
 *         长按时间就会变长（10ms 周期被挤成 50ms，长按阈值实际变成 5 倍）。
 *         这里由调用方传入真实 elapsed_ms，周期抖动不影响判定。
 *         而传入的 elapsed_ms 超过消抖时间时，直接判定稳定，不会漏掉快速点击。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ds_common.h"
#include "app_key.h"
// 功能包含
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "APP_KEY";

/** @brief 按键事件队列深度（深度 16 足够：人手最快也就 10 次/秒） */
#define APP_KEY_QUEUE_LEN   16

/*==============================================================================
 * 内部类型定义
 *============================================================================*/
/** @brief 按键内部状态 */
typedef enum {
    APP_KEY_STATE_NO   = 0,     /**< 未按下 */
    APP_KEY_STATE_YES  = 1,     /**< 已按下 */
    APP_KEY_STATE_LONG = 2,     /**< 长按已触发，等释放 */
} App_Key_State_t;

/**
 * @brief 按键实例结构体
 * @note  位域打包节省 RAM（8 个按键 × 约 20 字节，可忽略，但保持参考工程风格）
 */
typedef struct {
    uint8_t                  key_id;            /**< 逻辑按键 ID */
    const BSP_GPIO_Handle_t *gpio;              /**< BSP 具名句柄 */
    uint8_t                  used;              /**< 槽位是否启用 */

    uint8_t                  stable       : 1;  /**< 消抖后的稳定电平 */
    uint8_t                  last_raw     : 1;  /**< 上一次原始电平 */
    uint8_t                  active_level : 1;  /**< 有效电平（1=高电平按下） */
    uint8_t                  state        : 2;  /**< 状态机 */
    uint8_t                  reserved     : 3;

    uint32_t                 stable_ms;         /**< 电平已稳定持续的毫秒数 */
    uint32_t                 press_ms;          /**< 已按住的毫秒数 */
    uint16_t                 long_press_ms;     /**< 长按阈值 */
    uint16_t                 debounce_ms;       /**< 消抖时长 */
} App_Key_Key_t;

/*==============================================================================
 * 静态资源池
 *============================================================================*/
static App_Key_Key_t  app_key_pool[KEY_MAX_NUM];
static uint8_t        app_key_count  = 0;
static QueueHandle_t  app_key_queue  = NULL;

static uint32_t       key_drop_count = 0;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 根据 ID 查找按键实例（O(n)，n ≤ 8）
 */
static App_Key_Key_t *App_Key_GetById(uint8_t key_id)
{
    for (uint8_t i = 0; i < app_key_count; i++)
    {
        if (app_key_pool[i].used && app_key_pool[i].key_id == key_id)
            return &app_key_pool[i];
    }
    return NULL;
}

/**
 * @brief 发送按键事件到消息队列
 * @note  队列满时丢弃并计数，绝不阻塞扫描任务 —— 按键丢一两个事件无伤大雅，
 *        但阻塞扫描任务会让后续所有按键都失灵。
 */

static void App_Key_SendEvent(uint8_t key_id, App_Key_Event_t event)
{
    if (app_key_queue == NULL)
        return;

    /* ★★ 这里【不能】因为"消费者忙"就丢弃事件 —— 这是踩过的坑 ★★
     *
     * ★ 教训（两版都错，记录在这里防止再犯）★
     *
     *   第一版：忙就丢弃【所有】新事件。结果：
     *     松手（CLICK）事件被丢掉 → s_key1_recording 永远停在 1、
     *     录音永远不停 → 再按 KEY1 时 RecordStart 被拒（已在录音中）
     *     → 【第二次按 KEY1 完全无反应】。
     *
     *   第二版想改成"只丢重复事件"，但一看枚举就发现行不通：
     *     本模块只产生 DOWN / CLICK / LONG / LONG_UP 四种事件，
     *     【没有任何周期性重复事件】—— 见 app_key.c 顶部的状态图：
     *         APP_KEY_STATE_NO   --按下消抖稳定--> DOWN    --> YES
     *         APP_KEY_STATE_YES  --释放消抖稳定--> CLICK   --> NO
     *         APP_KEY_STATE_YES  --长按超时-----> LONG    --> LONG
     *         APP_KEY_STATE_LONG --释放--------> LONG_UP --> NO
     *     每一个都是【状态机的唯一次机会】，丢掉任何一个都会让按键状态
     *     与上层记录永久不一致（上层不会自己恢复）。
     *
     * ★ 真正的解法不在这里，而在"别让消费者长时间不消费" ★
     *   上传原本阻塞 Task_Key_Cmd 45 秒 ~ 3.75 分钟，队列（16 深）必然溢出。
     *   现在上传已改为交给独立的 worker 任务（见 task_key.c 的
     *   Task_Key_Upload），Task_Key_Cmd 立刻返回继续消费按键事件，
     *   队列就不会再溢出。所以这里保持"满了才丢 + 告警"的原始行为，
     *   而且那条告警如果出现，就说明 worker 方案没生效，是值得注意的信号。
     */    App_Key_EventPacket_t msg = { .key_id = key_id, .event = event };

    if (xQueueSend(app_key_queue, &msg, 0) != pdTRUE)
    {
        key_drop_count++;
        ESP_LOGW(TAG, "event queue full, drop key%u evt=%d (total drop %u)",
                 key_id, (int)event, (unsigned)key_drop_count);
    }
}

/**
 * @brief 消抖状态机核心
 * @param key        按键实例
 * @param raw        本次读到的原始电平
 * @param elapsed_ms 距上次调用的毫秒数
 * @retval 按键事件（APP_KEY_EVENT_NONE 表示无事件）
 *
 * @note 分两步，与参考工程一致：
 *       1. 消抖：同一电平持续 debounce_ms 才更新 stable
 *       2. 状态机：只用 stable 驱动，避免毛刺误触发
 */
static App_Key_Event_t App_Key_DebounceUpdate(App_Key_Key_t *key, uint8_t raw, uint32_t elapsed_ms)
{
    if (raw == 0xFF)
        return APP_KEY_EVENT_NONE;      /* BSP 层无效句柄 */

    /* ---------- 1. 消抖 ---------- */
    if (raw == key->last_raw)
    {
        key->stable_ms += elapsed_ms;

        if (key->stable_ms < key->debounce_ms)
            return APP_KEY_EVENT_NONE;

        key->stable = raw;              /* 只有这里才更新稳定值 */
    }
    else
    {
        key->last_raw  = raw;
        key->stable_ms = 0;

        /* ★ 若单次间隔已经超过消抖时间，直接认定稳定，避免快速点击被吃掉 */
        if (elapsed_ms >= key->debounce_ms)
        {
            key->stable_ms = key->debounce_ms;
            key->stable    = raw;
        }
        else
        {
            return APP_KEY_EVENT_NONE;
        }
    }

    /* ---------- 2. 状态机：只用 stable ---------- */
    switch (key->state)
    {
        case APP_KEY_STATE_NO:
            if (key->stable == key->active_level)
            {
                key->state    = APP_KEY_STATE_YES;
                key->press_ms = 0;
                return APP_KEY_EVENT_DOWN;
            }
            break;

        case APP_KEY_STATE_YES:
            if (key->stable == key->active_level)
            {
                key->press_ms += elapsed_ms;

                if (key->press_ms >= key->long_press_ms)
                {
                    key->state = APP_KEY_STATE_LONG;
                    return APP_KEY_EVENT_LONG;      /* 长按只触发一次 */
                }
            }
            else
            {
                key->state = APP_KEY_STATE_NO;

                if (key->press_ms < key->long_press_ms)
                    return APP_KEY_EVENT_CLICK;     /* 短按 */
                else
                    return APP_KEY_EVENT_LONG_UP;
            }
            break;

        case APP_KEY_STATE_LONG:
            if (key->stable != key->active_level)
            {
                key->state = APP_KEY_STATE_NO;
                return APP_KEY_EVENT_LONG_UP;
            }
            break;

        default:
            key->state = APP_KEY_STATE_NO;
            break;
    }

    return APP_KEY_EVENT_NONE;
}

/*==============================================================================
 * 初始化函数
 *============================================================================*/
/**
 * @brief 按键模块初始化
 */
void App_Key_Init(void)
{
    /* 只配置按键输入那一段引脚，不碰指示灯（指示灯归 APP_LED 初始化）。
     * ★ 早期版本这里调的是 BSP_GPIO_Init()（整张表），会和 LED 任务重复配置按键；
     *   拆成 InitInputs/InitOutputs 之后各管一段，语义清晰。 */
    BSP_GPIO_InitInputs();

    memset(app_key_pool, 0, sizeof(app_key_pool));
    app_key_count  = 0;
    key_drop_count = 0;

    if (app_key_queue == NULL)
    {
        app_key_queue = xQueueCreate(APP_KEY_QUEUE_LEN, sizeof(App_Key_EventPacket_t));
        if (app_key_queue == NULL)
            ESP_LOGE(TAG, "create event queue failed");
    }

    ESP_LOGI(TAG, "init done, queue depth = %d", APP_KEY_QUEUE_LEN);
}

/**
 * @brief 注册一个按键到静态资源池
 */
uint8_t App_Key_Register(uint8_t key_id,
                         const BSP_GPIO_Handle_t *gpio,
                         uint16_t long_press_ms,
                         uint8_t active_level,
                         uint16_t debounce_ms)
{
    /* ---------- 参数检查 ---------- */
    if (gpio == NULL || debounce_ms == 0)
        return 0;

    /* ---------- 查重 ---------- */
    if (App_Key_GetById(key_id) != NULL)
    {
        ESP_LOGW(TAG, "key_id %u already registered", key_id);
        return 0;
    }

    /* ---------- 容量检查 ---------- */
    if (app_key_count >= KEY_MAX_NUM)
        return 2;

    /* ---------- 必须是含输入位的脚 ---------- */
    if (!BSP_GPIO_IsInput(gpio))
    {
        ESP_LOGW(TAG, "key%u: IO%d is not an input, register rejected",
                 key_id, BSP_GPIO_GetNum(gpio));
        return 0;
    }

    /* ---------- 注册 ---------- */
    App_Key_Key_t *key = &app_key_pool[app_key_count];

    memset(key, 0, sizeof(*key));
    key->key_id        = key_id;
    key->gpio          = gpio;
    key->used          = 1;
    key->active_level  = active_level ? 1 : 0;
    key->long_press_ms = long_press_ms;
    key->debounce_ms   = debounce_ms;
    key->state         = APP_KEY_STATE_NO;

    /* 用当前真实电平作为初值，避免上电时按键已按下被误判成一次点击 */
    uint8_t now = BSP_GPIO_ReadLevel(gpio);
    key->last_raw  = (now == 0xFF) ? (key->active_level ? 0 : 1) : now;
    key->stable    = key->last_raw;
    key->stable_ms = debounce_ms;

    app_key_count++;

    ESP_LOGI(TAG, "key%u registered on IO%d (active %s, long %ums, debounce %ums)",
             key_id, BSP_GPIO_GetNum(gpio),
             key->active_level ? "HIGH" : "LOW",
             key->long_press_ms, key->debounce_ms);
    return 1;
}

/*==============================================================================
 * 更新按键状态
 *============================================================================*/
/**
 * @brief 更新所有已注册按键的状态
 */
void App_Key_Update(uint32_t elapsed_ms)
{
    if (elapsed_ms == 0)
        elapsed_ms = 1;     /* 防止除零/零累加导致状态机卡死 */

    for (uint8_t i = 0; i < app_key_count; i++)
    {
        App_Key_Key_t *key = &app_key_pool[i];
        if (!key->used)
            continue;

        uint8_t raw = BSP_GPIO_ReadLevel(key->gpio);
        App_Key_Event_t event = App_Key_DebounceUpdate(key, raw, elapsed_ms);

        if (event != APP_KEY_EVENT_NONE)
            App_Key_SendEvent(key->key_id, event);
    }
}

/*==============================================================================
 * 事件队列
 *============================================================================*/
/**
 * @brief 获取按键事件队列句柄
 */
QueueHandle_t App_Key_GetQueue(void)
{
    return app_key_queue;
}

/**
 * @brief 阻塞读取一个按键事件
 */
uint8_t App_Key_WaitEvent(App_Key_EventPacket_t *msg, uint32_t timeout_ms)
{
    if (app_key_queue == NULL || msg == NULL)
        return 0;

    TickType_t ticks = (timeout_ms == 0xFFFFFFFFu)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);

    return (xQueueReceive(app_key_queue, msg, ticks) == pdTRUE) ? 1 : 0;
}

/**
 * @brief 查询因队列满而丢弃的事件数
 */
uint32_t App_Key_GetDropCount(void)
{
    return key_drop_count;
}
