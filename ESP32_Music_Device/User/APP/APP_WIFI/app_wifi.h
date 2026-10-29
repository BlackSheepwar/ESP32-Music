/**
 * @file app_wifi.h
 * @brief WiFi 应用层（连接状态机 + 重连退避 + 状态指示灯联动）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块是 WiFi 的「大脑」，BSP_WIFI 只是「手脚」：
 *
 *          BSP_WIFI（bsp_wifi.c）  → 调 esp_wifi，把状态变化用回调抛上来
 *          APP_WIFI（本模块）      → 决定什么时候连、失败后等多久再连、灯怎么闪
 *          TASK_WIFI（task_wifi.c）→ 每 50ms 调一次 App_WIFI_Service() 推进状态机
 *
 *       ★ 重连策略（逻辑全在本模块，BSP 层不参与）：
 *
 *         连接中 ──成功──→ 已连接（灯常亮，之后每 10s 巡检一次）
 *            │
 *            └─失败/超时─→ 重试等待（灯慢闪）
 *                             │ 第 1~5 次：等 1s / 2s / 4s / 8s / 8s
 *                             │ 用尽 5 次
 *                             ↓
 *                          长退避（灯双闪后长灭）
 *                             等 5s / 10s / 20s / 30s …（封顶 30s，永不放弃）
 *
 *       ★ 为什么不用 ESP-IDF 自带的自动重连：见 bsp_wifi.c 顶部说明。
 *
 *       ★ 本模块不阻塞：App_WIFI_Service() 每次调用只做「看时间 → 该干嘛干嘛」，
 *         所有等待都靠时间戳比较实现，没有任何 vTaskDelay。
 *
 *       ★★ 任务上下文约定（审计修复 M2 / M3，调用本模块前请先看这一段）★★
 *
 *         1) BSP 层的状态回调 app_wifi_on_state() 运行在 ESP-IDF 的 WiFi 系统
 *            事件任务里，它的栈只有 CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE
 *            = 2304 字节，而且开了 CANARY 栈检查（溢出即 panic 重启）。
 *            所以该回调【只把状态投递进模块内部的事件信箱】：
 *            不做 GetInfo、不打日志、不切灯、不推进状态。
 *
 *         2) 真正的状态推进只发生在 App_WIFI_Service()（即 TASK_WIFI 任务）里：
 *            它开头先消费事件信箱，再跑下面的状态机。
 *            由此得到「共享状态只有一个写者」的性质 ——
 *            s_state / s_fail_count / s_backoff_level / s_wait_until_ms /
 *            s_connect_start_ms / s_ip_str / s_rssi 都不再存在跨任务写竞争。
 *
 *         3) ★ 调用约定 ★
 *              App_WIFI_Service()           必须只由 TASK_WIFI 一个任务调用（它写状态）
 *              App_WIFI_GetState/IsConnected/GetIP/GetRssi/GetFailCount
 *                                            可以在任意任务里读（安全性逐一见各函数注释）
 *              App_WIFI_RequestReconnect()   可以在任意任务里调（只置一个 volatile 标志）
 */

#ifndef __APP_WIFI_H__
#define __APP_WIFI_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief WiFi 应用层状态机状态
 */
typedef enum {
    APP_WIFI_ST_IDLE = 0,       // 未启动
    APP_WIFI_ST_CONNECTING,     // 连接中（含获取 IP）
    APP_WIFI_ST_CONNECTED,      // 已连接（有 IP）
    APP_WIFI_ST_RETRY_WAIT,     // 短重试等待
    APP_WIFI_ST_BACKOFF,        // 长退避等待
} App_WiFi_State_t;

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief WiFi 应用层初始化
 * @note  做三件事：
 *          1. 注册 BSP 层的状态回调
 *          2. 用 app_wifi_cfg.h 里的宏填好连接配置
 *          3. 把指示灯置为「灭」，等第一次 Service 才真正开始连
 *        ★ 必须在 App_LED_Init() 之后调用（要用到 LED 句柄）。
 * @retval 0: 成功
 * @retval -1: 失败（BSP 层初始化失败，串口有日志）
 */
int App_WiFi_Init(void);

/*==============================================================================
 * 周期服务
 *============================================================================*/
/**
 * @brief WiFi 状态机推进（建议 50ms 调一次）
 * @note  首次调用会自动发起连接（所以不需要单独的 Start 函数）。
 *        内部只在「到点了」才动作，绝大多数调用是直接返回的空转。
 *
 *        ★★ 审计修复 M2 / M3：本函数是【唯一的状态推进入口】★★
 *        它开头会把 BSP 事件任务投递进事件信箱的状态取出来处理
 *        （连接成功取 IP、失败计数、退避计时、切灯、打日志都在这里做），
 *        然后再推进 IDLE / CONNECTING / CONNECTED / RETRY_WAIT / BACKOFF 状态机。
 *        事件处理刻意放在状态机 switch 之前，这样事件引起的新状态能在同一轮被看到。
 *
 *        ★ 因此本函数【必须只由一个任务调用】（当前是 TASK_WIFI）。
 *          从两个任务同时调它会重新引入竞争（和本次修复的 M2 是同一类问题）。
 *          它内部会取一个很短的临界区来取走事件，但临界区外的状态推进没有加锁 ——
 *          设计前提就是单一调用者。
 */
void App_WiFi_Service(void);

/*==============================================================================
 * 状态查询
 *============================================================================*/
/**
 * @brief 查询应用层状态
 * @note  可在任意任务里调：内部是 volatile 单次读，最坏读到"上一轮"的值，
 *        不会读到撕裂值。写者只有 TASK_WIFI（经 App_WIFI_Service）。
 */
App_WiFi_State_t App_WiFi_GetState(void);

/**
 * @brief 是否已连接并拿到 IP（业务层发 HTTP 前必须检查这个）
 * @retval 1: 可以发请求
 * @retval 0: 网络还没就绪
 */
uint8_t App_WiFi_IsConnected(void);

/**
 * @brief 获取本机 IP 字符串
 * @retval 非 NULL: 形如 "192.168.1.23" 的静态字符串（未连接时是 "0.0.0.0"）
 * @note  返回的是模块内部的静态缓冲，不要 free。
 *        ★ 安全性（如实说明，不粉饰）★ 写者只有 TASK_WIFI，但写入是多次写
 *        （strncpy），如果从【别的任务】调用本函数，理论上可能读到"写了一半"
 *        的字符串（表现为日志里出现错乱的 IP；不会越界、不会崩）。
 *        当前工程里两个调用点都在 TASK_WIFI 任务内（探活日志 / 上传失败日志），
 *        与服务任务是同一个任务，所以实际不构成并发。
 *        以后若有别的任务要读，请改为调用方提供缓冲区、由服务任务做一次拷贝。
 */
const char *App_WiFi_GetIP(void);

/**
 * @brief 获取信号强度
 * @retval 负数 dBm（未连接时返回 0）
 * @note  int8_t 的单字节读写在 Xtensa 上原子，任意任务可安全调用。
 */
int8_t App_WiFi_GetRssi(void);

/**
 * @brief 查询已连续失败次数（诊断用）
 * @note  uint8_t 单字节读，任意任务可安全调用。
 */
uint8_t App_WiFi_GetFailCount(void);

/**
 * @brief 请求立即重连（会重置退避计数）
 * @note  给按键用：以后做「长按 KEY1 强制重连」时的入口。
 *        任意任务可调用：只置一个 volatile 标志，真正的清零与重连由
 *        App_WIFI_Service() 在 TASK_WIFI 里完成。
 */
void App_WiFi_RequestReconnect(void);

#endif /* __APP_WIFI_H__ */
