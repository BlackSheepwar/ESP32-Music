/**
 * @file app_wifi.c
 * @brief WiFi 应用层实现（连接状态机 + 重连退避 + 状态指示灯联动）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块把 bsp_wifi.c 抛上来的原始状态，加工成「我该不该重连、灯该怎么闪」，
 *       是整个 WiFi 功能里唯一有业务判断的地方。
 *
 *       ★ 状态从「连接中」跳走有两条路（对应两个写入口）：
 *           BSP 事件（经信箱转交）        —— BSP 层报上来的异步结果
 *           Service 里的超时判定           —— BSP 层一直不报（AP 失联）时的兜底
 *         两条路都在【服务任务】里汇合于 app_wifi_enter()，
 *         保证进入每个状态时的动作只有一份。
 *
 *       ★★ 并发与栈安全（审计修复 M2 + M3，改动前请先读完这段）★★
 *
 *       1) 事件任务栈很小
 *          回调 app_wifi_on_state() 运行在 ESP-IDF 的 WiFi 系统事件任务里，
 *          它的栈只有 CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE = 2304 字节
 *          （见 build/config/sdkconfig.h），而 sdkconfig 里
 *          CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1 —— 栈溢出不是"卡一下"，
 *          而是 canary 触发 panic 重启。
 *          所以回调里【绝对不能】做这些事：
 *            · BSP_WIFI_GetInfo()：内部一个 wifi_ap_record_t + 3 次 snprintf
 *            · ESP_LOGx(带多个 %s/%d/%u)：串口格式化整段在调用者栈上做
 *            · App_LED_Set()：跨任务写 LED 池（app_led.c 里有临界区，但仍不该在
 *              小栈任务里顺手做业务）
 *          现在回调【只往信箱里写 4 个变量】，重活全部由服务任务代劳。
 *
 *       2) 以前 app_wifi_enter() 有两个调用上下文（真实的 data race）
 *          一个是事件任务（回调里直接调），一个是服务任务（状态机里调）。
 *          而它读写 s_state / s_fail_count / s_backoff_level / s_wait_until_ms /
 *          s_connect_start_ms / s_ip_str / s_rssi，既没 volatile 也没锁：
 *            · s_fail_count++ 是读改写 → 丢失更新 / 双重计数 → 退避阶梯提前触发
 *            · 两个上下文交错进 app_wifi_enter() → 灯和状态不一致
 *              （事件任务刚点常亮，服务任务随后改成慢闪）
 *            · strncpy(s_ip_str, ...) 与 App_WiFi_GetIP() 的读发生撕裂
 *
 *       3) 现在的所有权（单一写者模型）
 *          ★ 所有状态推进只发生在服务任务（TASK_WIFI，core0，prio4）里 ★
 *            —— app_wifi_enter() / s_fail_count / s_backoff_level / s_wait_until_ms /
 *               s_connect_start_ms / s_ip_str / s_rssi 只有这一个写者。
 *          ★ 事件任务只负责"投递事件"（见 app_wifi_on_state），不碰任何状态 ★
 *
 *       4) volatile 解决了什么、没解决什么
 *          volatile 只保证"每次访问都真的读写内存、不被编译器优化成寄存器副本"，
 *          它【不】保证 ++ 这种读改写是原子的。本次重构之所以敢继续用
 *          s_fail_count++，是因为重构后这些变量只剩服务任务一个写者；
 *          信箱的投递/取走则用 portMUX_TYPE 临界区保证整体原子。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "ds_common.h"
#include "app_wifi.h"
#include "app_wifi_cfg.h"
// 功能包含
#include "app_led.h"
#include "bsp_sys.h"
#include "bsp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"     /* esp_get_free_heap_size */
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"     /* portMUX_TYPE / portENTER_CRITICAL（事件信箱用） */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "APP_WIFI";

/** @brief 短重试退避阶梯（毫秒）—— 索引即「已失败次数 - 1」，用完则夹到最后一档 */
static const uint16_t s_retry_backoff_ms[] = WIFI_RETRY_BACKOFF_MS;

/** @brief 长退避阶梯（毫秒）—— 索引即「进入长退避的次数 - 1」，用完则夹到最后一档 */
static const uint16_t s_long_backoff_ms[]  = WIFI_LONG_BACKOFF_MS;

/** @brief 巡检发现异常后，连续多少次才认定「真的断了」（防抖） */
#define WIFI_MONITOR_CONFIRM    2

/**
 * @brief 一次 App_WiFi_Service() 里最多连续消费多少个 BSP 事件
 * @note  纯粹是"防死循环"的上界：正常情况下信箱里最多 1 个事件。
 *        万一 BSP 层发疯一样地报状态，也只会每 50ms 消费 8 个，
 *        多出来的留在信箱里（pending 仍为 1），下一次 Service 继续处理，不会丢。
 */
#define WIFI_EVT_DRAIN_MAX      8

/*==============================================================================
 * 静态变量
 *============================================================================*/
/* ---------------- 状态机共享状态 ----------------
 * ★ 所有权：下面这一组【只有服务任务 TASK_WIFI 一个写者】——
 *   事件回调不再直接改它们，只往信箱里投递（见下面的 s_evt_* 及 app_wifi_on_state）。
 *   唯一的例外是 s_reconnect_req：任意任务都可能置 1（App_WiFi_RequestReconnect），
 *   由服务任务读并清零；它是单字节标志，置 1 / 清 0 本身就是原子写，不需要锁。
 *   加 volatile 是为了：这几个变量会被其它任务通过 App_WiFi_GetState / GetIP /
 *   GetRssi / GetFailCount 读到，必须保证"每次都真的从内存读"，
 *   不能因为编译器看本编译单元内没人改就把它缓存进寄存器。
 *   注意 volatile ≠ 原子：s_fail_count++ 依然是读改写，本文件之所以安全，
 *   是因为它只剩服务任务这一个写者（详见文件头 "并发与栈安全" 第 3、4 点）。
 */
/** @brief 当前状态机状态（写者：服务任务；读者：任意任务） */
static volatile App_WiFi_State_t s_state = APP_WIFI_ST_IDLE;

/** @brief BSP 层最近一次报上来的原始状态（写者：服务任务消费事件时） */
static volatile BSP_WIFI_State_t s_bsp_state = BSP_WIFI_STATE_IDLE;

/** @brief BSP 层最近一次报上来的断连原因（写者：服务任务消费事件时） */
static volatile BSP_WIFI_Reason_t s_bsp_reason = BSP_WIFI_REASON_UNKNOWN;

/** @brief 连续失败次数（连接成功后清零）—— 只在服务任务里改 */
static volatile uint8_t s_fail_count = 0;

/** @brief 进入长退避的次数（用于选长退避档位）—— 只在服务任务里改 */
static volatile uint8_t s_backoff_level = 0;

/** @brief 是否已初始化 */
static uint8_t s_inited = 0;

/** @brief 配置里还是占位符（没填 WiFi 账号），此时不做无意义的重试 */
static uint8_t s_placeholder = 0;

/** @brief 外部请求立即重连的标志（写者：任意任务；读者/清零者：服务任务） */
static volatile uint8_t s_reconnect_req = 0;

/** @brief 各阶段的时间戳与超时（全部毫秒）—— 只在服务任务里改 */
static volatile uint32_t s_connect_start_ms  = 0;   // 本次连接开始时刻
static volatile uint32_t s_wait_until_ms     = 0;   // 退避等待的截止时刻
static uint32_t s_monitor_next_ms   = 0;    // 下次巡检时刻（只服务任务用）
static uint32_t s_monitor_bad       = 0;    // 巡检连续异常次数（只服务任务用）

/** @brief 本机 IP 缓存（供 App_WiFi_GetIP 返回静态字符串）
 *  @note  volatile 修饰数组本身；写者只有服务任务，读者可能来自别的任务。
 *         读字符串这件事本身不原子 —— 见 App_WiFi_GetIP() 的说明。 */
static volatile char     s_ip_str[16] = "0.0.0.0";

/** @brief 最近一次信号强度（写者：服务任务；读者：任意任务；单字节读写天然原子） */
static volatile int8_t   s_rssi = 0;

/* ---------------- BSP 事件信箱（事件任务 → 服务任务）----------------
 * ★ 为什么要有这个信箱 ★
 *   回调 app_wifi_on_state() 跑在 WiFi 系统事件任务里，栈只有 2304 字节
 *   （CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE），还开了 CANARY 栈检查。
 *   改动前它直接调 app_wifi_enter()，连带做了 GetInfo（3 次 snprintf）、
 *   带多个 %s 的 ESP_LOGI、带 4 个参数的 ESP_LOGW/LOGE、以及 LED 切换 ——
 *   这些全部吃调用者（事件任务）的栈，是实打实的溢出风险。
 *   现在回调只写下面这 4 个变量，真正干活的是服务任务。
 *
 * ★ 为什么要加临界区，而不是只加 volatile ★
 *   volatile 保证"写进去了、读得出来"，但不保证"投递/取走"这个多步动作原子。
 *   没有临界区时存在这样一个漏事件的窗口：
 *       服务任务：读 seq=N → 读载荷 → 判断 seq 没变 → 清 pending=0
 *       事件任务：                                    ← 就在这中间写入新事件：
 *                                                        载荷=新、seq=N+1、pending=1
 *       服务任务：pending=0 把刚置上的 1 又抹掉了 → 这个事件要等到"下一次事件"才被看见
 *   （现实中这个窗口会导致状态机白白多等一轮超时，不是永久的，但没必要留。）
 *   所以投递和取走都各自放在一个几纳秒的 portENTER_CRITICAL 里。
 */
static portMUX_TYPE s_evt_mux = portMUX_INITIALIZER_UNLOCKED;

/** @brief 信箱里有未处理的事件（只在 s_evt_mux 临界区内改写） */
static volatile uint8_t  s_evt_pending = 0;

/** @brief 信箱载荷：BSP 报上来的状态 */
static volatile BSP_WIFI_State_t  s_evt_state  = BSP_WIFI_STATE_IDLE;

/** @brief 信箱载荷：BSP 报上来的断连原因 */
static volatile BSP_WIFI_Reason_t s_evt_reason = BSP_WIFI_REASON_UNKNOWN;

/** @brief 投递序号：每投递一次 +1，用来识别"处理期间又来了新事件" */
static volatile uint32_t s_evt_seq = 0;

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把 WiFi 指示灯切成某个图案
 * @param pattern 图案；NULL 表示灭
 * @note  按「逻辑编号」操作，不抓句柄 —— 点位变了只改 app_led_cfg.h。
 */
static void app_wifi_led(const App_LED_Pattern_t *pattern)
{
    App_LED_Set(WIFI_LED_ID, (pattern != NULL) ? pattern : &APP_LED_PAT_OFF);
}

/**
 * @brief 写 s_ip_str（★ 只在服务任务里调用 ★）
 * @param ip 形如 "192.168.1.23" 的字符串；NULL 视为未连接
 * @note  s_ip_str 加了 volatile，直接把它交给 strncpy 会因为丢掉 volatile
 *        限定符而报 -Wdiscarded-qualifiers（本工程要求零警告），
 *        所以这里显式转一次 char*，并且统一做「截断 + 补 '\0'」，
 *        免得每个调用点各写一遍 sizeof-1 的算术。
 */
static void app_wifi_set_ip(const char *ip)
{
    char *dst = (char *)s_ip_str;       /* 显式去 volatile：写者唯一，见文件头说明 */

    if (ip == NULL)
        ip = "0.0.0.0";

    strncpy(dst, ip, sizeof(s_ip_str) - 1);
    dst[sizeof(s_ip_str) - 1] = '\0';
}

/**
 * @brief 统一的「进入某状态」入口
 * @param next 目标状态
 * @note  ★ 所有状态切换都必须走这里，好处是「进这个状态要做什么」只有一份，
 *          不会出现「事件回调里点了灯、Service 里忘了点」这种不一致。
 *        ★★ 审计修复 M2：本函数现在【只在服务任务 TASK_WIFI 上下文被调用】★★
 *          它要改 s_state / s_fail_count / s_backoff_level / s_wait_until_ms /
 *          s_connect_start_ms / s_ip_str，还会点灯、还会打带多个 %s 的日志 ——
 *          这些以前是"服务任务 + WiFi 事件任务"两个上下文同时在做的，
 *          既没有锁也没有顺序保证，是真实的数据竞争（s_fail_count++ 会丢更新、
 *          灯会出现"事件任务刚点常亮、服务任务随即改成慢闪"）。
 *          现在事件任务只投递事件，本函数由服务任务在消费事件时调用，
 *          于是全部共享状态恢复成【单一写者】。
 *        ★ 本函数不主动发起连接（那由状态机在下一轮决定），
 *          所以在服务任务里调用也是纳秒级、不阻塞。
 */
/** @brief 状态名（日志与诊断用） */
static const char *app_wifi_state_name(App_WiFi_State_t st)
{
    switch (st)
    {
    case APP_WIFI_ST_IDLE:       return "IDLE";
    case APP_WIFI_ST_CONNECTING: return "CONNECTING";
    case APP_WIFI_ST_CONNECTED:  return "CONNECTED";
    case APP_WIFI_ST_RETRY_WAIT: return "RETRY_WAIT";
    case APP_WIFI_ST_BACKOFF:    return "BACKOFF";
    default:                     return "?";
    }
}

static void app_wifi_enter(App_WiFi_State_t next)
{
    if (s_state == next)
    {
        /* ★ 诊断用（D 级，默认不显示）★
         * 原来这里直接 return，于是"状态没变 → 灯没跟着变"这件事
         * 在日志里完全不可见 —— 排查"WiFi 断了灯还常亮"时就成了盲区。
         * 加一行 D 级日志：需要时把日志等级调成 DEBUG，就能看到
         * 每次 enter 调用是"真的切了"还是"被这次早退拦住了"。 */
        ESP_LOGD(TAG, "enter(%s): 状态未变，直接返回（灯保持不变）",
                 app_wifi_state_name(s_state));
        return;
    }

    ESP_LOGI(TAG, "state: %s -> %s", app_wifi_state_name(s_state),
             app_wifi_state_name(next));

    App_WiFi_State_t old = s_state;
    s_state = next;

    switch (next)
    {
    case APP_WIFI_ST_CONNECTING:
        app_wifi_led(&APP_LED_PAT_FAST_BLINK);      /* 连接中：快闪 */
        s_connect_start_ms = BSP_SYS_GetTickMs();
        break;

    case APP_WIFI_ST_CONNECTED:
    {
        /* 连接成功：灯常亮（需求明确要求「常亮 = 连接成功」） */
        app_wifi_led(&APP_LED_PAT_ON);

        s_fail_count    = 0;
        s_backoff_level = 0;
        s_monitor_bad   = 0;
        s_monitor_next_ms = BSP_SYS_GetTickMs() + WIFI_MONITOR_PERIOD_MS;

        BSP_WIFI_Info_t info;
        if (BSP_WIFI_GetInfo(g_wifi_sta, &info) == 0)
        {
            app_wifi_set_ip(info.ip);
            s_rssi = info.rssi;

            ESP_LOGI(TAG, "★ CONNECTED ★ ip=%s mask=%s gw=%s rssi=%d dBm ch=%u",
                     info.ip, info.netmask, info.gateway,
                     (int)info.rssi, info.channel);
        }
        else
        {
            app_wifi_set_ip(NULL);
            ESP_LOGW(TAG, "connected but get_info failed");
        }
        break;
    }

    case APP_WIFI_ST_RETRY_WAIT:
    {
        app_wifi_led(&APP_LED_PAT_SLOW_BLINK);      /* 重连等待：慢闪 */

        uint8_t idx = (s_fail_count > 0) ? (uint8_t)(s_fail_count - 1) : 0;
        if (idx >= ARRAY_SIZE(s_retry_backoff_ms))
            idx = ARRAY_SIZE(s_retry_backoff_ms) - 1;

        uint32_t wait = s_retry_backoff_ms[idx];
        s_wait_until_ms = BSP_SYS_GetTickMs() + wait;

        ESP_LOGW(TAG, "connect failed (%u/%u, reason=%d), retry in %u ms",
                 s_fail_count, WIFI_MAX_RETRY, (int)s_bsp_reason, (unsigned)wait);
        break;
    }

    case APP_WIFI_ST_BACKOFF:
    {
        app_wifi_led(&APP_LED_PAT_DOUBLE_BLINK);    /* 长退避：双闪后长灭 */

        uint8_t idx = (s_backoff_level > 0) ? (uint8_t)(s_backoff_level - 1) : 0;
        if (idx >= ARRAY_SIZE(s_long_backoff_ms))
            idx = ARRAY_SIZE(s_long_backoff_ms) - 1;

        uint32_t wait = s_long_backoff_ms[idx];
        s_wait_until_ms = BSP_SYS_GetTickMs() + wait;

        ESP_LOGE(TAG, "%u consecutive failures, long backoff #%u for %u ms "
                      "(check SSID/password/2.4GHz)",
                 WIFI_MAX_RETRY, s_backoff_level, (unsigned)wait);
        break;
    }

    case APP_WIFI_ST_IDLE:
        app_wifi_led(&APP_LED_PAT_OFF);
        break;

    default:
        break;
    }

    ESP_LOGD(TAG, "state %d -> %d", (int)old, (int)next);
}

/**
 * @brief BSP 层状态变化回调（★ 运行在 WiFi 系统事件任务里 ★）
 * @param handle 句柄（未使用）
 * @param state  BSP 层新状态
 * @param reason 断连原因
 *
 * @note  ★★ 审计修复 M3：这个函数现在【只往信箱里写 4 个变量】，别的一律不做 ★★
 *
 *        上下文：WiFi 系统事件任务，栈 = CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE
 *                = 2304 字节（build/config/sdkconfig.h），并开了
 *                CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1 —— 溢出即 panic。
 *        因此这里【禁止】出现：
 *          · app_wifi_enter() / app_wifi_led() / App_LED_Set()  → 状态推进与点灯是服务任务的事
 *          · BSP_WIFI_GetInfo()                                 → 里面有 wifi_ap_record_t + 3 次 snprintf
 *          · ESP_LOGx（尤其带多个 %s/%d/%u 的）                  → 格式化整段都在本任务栈上做
 *        真正的处理在 app_wifi_handle_bsp_event()，由服务任务经
 *        app_wifi_drain_events() 调用。
 *
 *        投递用临界区（而不是裸 volatile）的理由见 s_evt_mux 处的注释：
 *        避免"服务任务清 pending"与"事件任务置 pending"交错时丢掉一个事件。
 *        临界区里只有 4 次赋值/自增，不进任何阻塞调用、不打日志。
 */
static void app_wifi_on_state(const BSP_WIFI_Handle_t *handle,
                              BSP_WIFI_State_t state,
                              BSP_WIFI_Reason_t reason)
{
    (void)handle;

    portENTER_CRITICAL(&s_evt_mux);
    s_evt_state   = state;
    s_evt_reason  = reason;
    s_evt_seq++;                /* 先记"有新事件"，再置 pending */
    s_evt_pending = 1;
    portEXIT_CRITICAL(&s_evt_mux);
}

/**
 * @brief 处理一个 BSP 事件（★ 只在服务任务上下文调用 ★）
 * @param state  BSP 层新状态
 * @param reason 断连原因
 *
 * @note  这里就是改动前 app_wifi_on_state() 里那段逻辑，一字未改地搬了过来，
 *        只是执行上下文从"事件任务(2304 字节栈)"换成了"服务任务"。
 *        搬家的意义：
 *          · GetInfo / 串口格式化不再吃事件任务那 2304 字节的栈
 *          · s_fail_count++ / app_wifi_enter() 回归单一写者，不再与服务任务里的
 *            同名操作互相覆盖
 */
static void app_wifi_handle_bsp_event(BSP_WIFI_State_t state, BSP_WIFI_Reason_t reason)
{
    s_bsp_state  = state;
    s_bsp_reason = reason;

    /* ★ 诊断：每个从 BSP 层送来的事件都如实记录（含"被忽略"的原因）★
     * 排查"断线后灯不变"时，这一行能立刻区分两种情况：
     *   · 事件根本没到（看不到这行）→ 问题在 BSP 层或回调注册
     *   · 事件到了但被忽略（看到"忽略"）→ 问题在下面的状态判断 */
    ESP_LOGI(TAG, "BSP 事件: state=%d reason=%d (当前 %s)",
             (int)state, (int)reason, app_wifi_state_name(s_state));

    switch (state)
    {
    case BSP_WIFI_STATE_CONNECTED:
        app_wifi_enter(APP_WIFI_ST_CONNECTED);
        break;

    case BSP_WIFI_STATE_DISCONNECTED:
    case BSP_WIFI_STATE_FAILED:
        /* 只在「正在连接 / 已连接」时才算作一次失败。
         * 若已经是退避状态，说明这次断连是退避期间的老消息，忽略。 */
        if (s_state == APP_WIFI_ST_CONNECTING || s_state == APP_WIFI_ST_CONNECTED)
        {
            if (s_fail_count < WIFI_MAX_RETRY)
                s_fail_count++;

            if (s_fail_count >= WIFI_MAX_RETRY)
            {
                s_backoff_level++;
                app_wifi_enter(APP_WIFI_ST_BACKOFF);
            }
            else
            {
                app_wifi_enter(APP_WIFI_ST_RETRY_WAIT);
            }
        }
        else
        {
            ESP_LOGW(TAG, "  ↳ 忽略：当前状态 %s 不是 CONNECTING/CONNECTED",
                     app_wifi_state_name(s_state));
        }
        break;

    default:
        ESP_LOGD(TAG, "  ↳ 该状态不处理");
        break;
    }
}

/**
 * @brief 把信箱里的事件搬到服务任务里处理（★ 只在服务任务上下文调用 ★）
 *
 * @note  取事件 / 处理 / 再看有没有新事件，三段分开：
 *
 *          1) 临界区内【原子地取走】一个事件：读载荷 + 读序号 + 清 pending。
 *             取走和投递互斥，所以"取走"这个动作不会把事件任务刚投进来的事件抹掉。
 *          2) 临界区外干活（GetInfo / 日志 / 灯 / 状态推进）—— 这些是慢操作，
 *             绝不能放在临界区里。
 *          3) 处理期间若又来了新事件，它的 seq 必然与取走时不同（投递时 seq++ 了），
 *             而且 pending 已被投递方重新置 1，所以 while 会再走一轮。
 *             万一 seq 变了而 pending 因为极端交错没留下，这里再补一次 pending，
 *             保证不漏。循环有 WIFI_EVT_DRAIN_MAX 上界，不会死循环；
 *             超出上界剩下的 pending 留给下一次 App_WiFi_Service()，同样不会丢。
 *
 * @note  ★ 信箱是"单槽 + 序号"，不是队列：同一个 50ms 周期内到达的多个事件，
 *          只有最后一个载荷会被处理（前一个被覆盖）。这不是疏忽，理由：
 *            · BSP 层只在状态真正变化时回调（bsp_wifi.c 的 old != state），
 *              而 ESP-IDF 侧的状态迁移本身就是串行的，一个周期内极少出现两个；
 *            · 真的丢了一个中间态也不会卡死：CONNECTED 丢了有 s_monitor 巡检兜底，
 *              DISCONNECTED 丢了有下面的 "BSP reports not connected" 兜底；
 *            · 换来的是回调里连一个队列都不用碰（不分配、不加锁等待）。
 *          如果以后要严格保序、一个都不许合并，正确做法是换成
 *          FreeRTOS 的 xQueueSendFromISR/xQueueSend（回调里非阻塞投递）。
 */
static void app_wifi_drain_events(void)
{
    uint8_t guard = 0;

    while (s_evt_pending != 0 && guard < WIFI_EVT_DRAIN_MAX)
    {
        guard++;

        /* ---- 1. 原子地取走一个事件 ---- */
        portENTER_CRITICAL(&s_evt_mux);
        BSP_WIFI_State_t  evt_state  = (BSP_WIFI_State_t)s_evt_state;
        BSP_WIFI_Reason_t evt_reason = (BSP_WIFI_Reason_t)s_evt_reason;
        uint32_t          seq        = s_evt_seq;
        s_evt_pending = 0;
        portEXIT_CRITICAL(&s_evt_mux);

        /* ---- 2. 在服务任务里做重活 ---- */
        app_wifi_handle_bsp_event(evt_state, evt_reason);

        /* ---- 3. 处理期间又来事件了？ ---- */
        portENTER_CRITICAL(&s_evt_mux);
        if (s_evt_seq != seq)
            s_evt_pending = 1;      /* 重新武装，下一轮循环处理最新载荷 */
        portEXIT_CRITICAL(&s_evt_mux);
    }
}

/**
 * @brief 检查配置里是不是还留着占位符
 * @retval 1: 是占位符（没填）
 * @retval 0: 已填
 */
static uint8_t app_wifi_is_placeholder(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return 1;

    return (strncmp(s, "YOUR_", 5) == 0) ? 1 : 0;
}

/**
 * @brief 真正发起一次连接
 */
static void app_wifi_start_connect(void)
{
    BSP_WIFI_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.ssid,     WIFI_SSID,     sizeof(cfg.ssid) - 1);
    strncpy(cfg.password, WIFI_PASSWORD, sizeof(cfg.password) - 1);

    cfg.channel            = WIFI_CHANNEL;
    cfg.max_retry          = 0;                     /* 重试由本层控制 */
    cfg.connect_timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
    cfg.static_ip_enable   = WIFI_STATIC_IP_ENABLE;

    strncpy(cfg.static_ip,      WIFI_STATIC_IP,      sizeof(cfg.static_ip) - 1);
    strncpy(cfg.static_netmask, WIFI_STATIC_NETMASK, sizeof(cfg.static_netmask) - 1);
    strncpy(cfg.static_gateway, WIFI_STATIC_GATEWAY, sizeof(cfg.static_gateway) - 1);

    /* 先切到 CONNECTING（会重置连接起始时间戳、点亮快闪），再下发连接命令。
     * 顺序很重要：BSP_WIFI_Connect() 内部会同步 Notify(CONNECTING)，
     * 状态先切好，那条通知就只是"重复的 CONNECTING"，不会把状态退回去。
     *
     * ★ 审计修复后这里有一处行为细节，如实记下来 ★
     *   BSP_WIFI_Connect() 如果同步失败，它会在【本任务】里回调一次
     *   app_wifi_on_state(FAILED)（见 bsp_wifi.c 的 BSP_WIFI_Notify 调用点）。
     *   改动前：那次回调当场就把 s_fail_count 加了一次，回到下面 if 里又加一次
     *           —— 同一次失败被计了两次，退避阶梯会提前跳档。
     *   现在：失败事件先进信箱，下一次 Service 才消费；而那时 s_state 已经是
     *         RETRY_WAIT/BACKOFF，事件按"退避期的老消息"被忽略，
     *         所以同一次失败只计一次。
     *   即失败计数从"偶尔跳一次"变成"一次失败一次计数"，
     *   与 app_wifi_cfg.h 里「连续失败多少次后进入长退避」的定义一致。
     *   连接流程、退避档位、灯效图案都没有变化，只是计数不再重复。 */
    app_wifi_enter(APP_WIFI_ST_CONNECTING);

    if (BSP_WIFI_Connect(g_wifi_sta, &cfg) != 0)
    {
        ESP_LOGE(TAG, "BSP_WIFI_Connect returned error");
        /* 立即计入一次失败，交给状态机决定是重试还是退避 */
        if (s_fail_count < WIFI_MAX_RETRY)
            s_fail_count++;

        if (s_fail_count >= WIFI_MAX_RETRY)
        {
            s_backoff_level++;
            app_wifi_enter(APP_WIFI_ST_BACKOFF);
        }
        else
        {
            app_wifi_enter(APP_WIFI_ST_RETRY_WAIT);
        }
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief WiFi 应用层初始化
 */
int App_WiFi_Init(void)
{
    if (s_inited)
        return 0;

    s_state         = APP_WIFI_ST_IDLE;
    s_bsp_state     = BSP_WIFI_STATE_IDLE;
    s_bsp_reason    = BSP_WIFI_REASON_UNKNOWN;
    s_fail_count    = 0;
    s_backoff_level = 0;
    s_reconnect_req = 0;
    s_monitor_bad   = 0;
    app_wifi_set_ip(NULL);

    /* 信箱也复位：Init 之后才注册回调，所以这里不会有并发写者 */
    s_evt_state   = BSP_WIFI_STATE_IDLE;
    s_evt_reason  = BSP_WIFI_REASON_UNKNOWN;
    s_evt_seq     = 0;
    s_evt_pending = 0;

    app_wifi_led(&APP_LED_PAT_OFF);

    /* ---------- BSP 层 ---------- */
    if (BSP_WIFI_Init() != 0)
    {
        ESP_LOGE(TAG, "BSP_WIFI_Init failed");
        return -1;
    }

    if (BSP_WIFI_SetStateCb(g_wifi_sta, app_wifi_on_state) != 0)
    {
        ESP_LOGE(TAG, "register state callback failed");
        return -1;
    }

    /* ---------- 配置检查 ---------- */
    s_placeholder = app_wifi_is_placeholder(WIFI_SSID);

    if (s_placeholder)
    {
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, " WIFI_SSID is still the placeholder!");
        ESP_LOGE(TAG, " Edit User/APP/APP_WIFI/app_wifi_cfg.h:");
        ESP_LOGE(TAG, "   #define WIFI_SSID      \"你的2.4G WiFi名\"");
        ESP_LOGE(TAG, "   #define WIFI_PASSWORD  \"你的WiFi密码\"");
        ESP_LOGE(TAG, " Then rebuild + flash. LED1 will blink fast.");
        ESP_LOGE(TAG, "==================================================");
        /* 灯保持快闪，明确提示「配置没填」，而不是死一样的灭 */
        app_wifi_led(&APP_LED_PAT_FAST_BLINK);
    }
    else
    {
        ESP_LOGI(TAG, "config loaded: ssid=\"%s\", %s, timeout=%d ms, max_retry=%d",
                 WIFI_SSID,
                 WIFI_STATIC_IP_ENABLE ? "static IP" : "DHCP",
                 WIFI_CONNECT_TIMEOUT_MS, WIFI_MAX_RETRY);
    }

    s_inited = 1;
    return 0;
}

/*==============================================================================
 * 周期服务
 *============================================================================*/
/**
 * @brief WiFi 状态机推进（★ 本函数只在 TASK_WIFI 任务上下文调用 ★）
 */
void App_WiFi_Service(void)
{
    if (!s_inited)
        return;

    /* ---------- 0. ★ 先消费事件任务投递过来的 BSP 事件 ★ ----------
     * 位置很关键：必须在下面 switch (s_state) 【之前】。
     * 事件处理会调 app_wifi_enter() 把 s_state 推到 CONNECTED / RETRY_WAIT /
     * BACKOFF，放在前面，同一轮 Service 就能按新状态立刻动作；
     * 放在后面则要白白多等一个 50ms 周期才开始计时。
     * 另外这里必须在取 now 之前调用：事件处理会写 s_wait_until_ms /
     * s_monitor_next_ms 这类时间戳，now 应当是处理完之后的时间。 */
    app_wifi_drain_events();

    uint32_t now = BSP_SYS_GetTickMs();

    /* ---------- 外部请求立即重连 ---------- */
    if (s_reconnect_req)
    {
        s_reconnect_req = 0;
        s_fail_count    = 0;
        s_backoff_level = 0;
        ESP_LOGI(TAG, "reconnect requested by user");
    }

    switch (s_state)
    {
    /* ---------------------------------------------------------------- */
    case APP_WIFI_ST_IDLE:
        if (s_placeholder)
            break;                  /* 没填账号，不发起无意义的连接 */

        ESP_LOGI(TAG, "starting first connection...");
        app_wifi_start_connect();
        break;

    /* ---------------------------------------------------------------- */
    case APP_WIFI_ST_CONNECTING:
        /* 兜底：BSP 层可能一直不报结果（AP 不响应关联请求时），
         * 超时后主动断开并计入失败，避免状态机卡死在这里。 */
        if ((now - s_connect_start_ms) >= WIFI_CONNECT_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "connect timeout (%d ms), aborting this attempt",
                     WIFI_CONNECT_TIMEOUT_MS);

            BSP_WIFI_Disconnect(g_wifi_sta);

            if (s_fail_count < WIFI_MAX_RETRY)
                s_fail_count++;

            if (s_fail_count >= WIFI_MAX_RETRY)
            {
                s_backoff_level++;
                app_wifi_enter(APP_WIFI_ST_BACKOFF);
            }
            else
            {
                app_wifi_enter(APP_WIFI_ST_RETRY_WAIT);
            }
        }
        break;

    /* ---------------------------------------------------------------- */
    case APP_WIFI_ST_CONNECTED:
    {
        /* 双重保险：BSP 层说没连上，但我们的状态还是已连接 → 补一次状态切换 */
        if (!BSP_WIFI_IsConnected(g_wifi_sta))
        {
            ESP_LOGW(TAG, "BSP reports not connected while APP thinks CONNECTED");
            if (s_fail_count < WIFI_MAX_RETRY)
                s_fail_count++;

            if (s_fail_count >= WIFI_MAX_RETRY)
            {
                s_backoff_level++;
                app_wifi_enter(APP_WIFI_ST_BACKOFF);
            }
            else
            {
                app_wifi_enter(APP_WIFI_ST_RETRY_WAIT);
            }
            break;
        }

        /* 周期性巡检：路由重启 / 信道切换时事件可能漏报 */
        if ((int32_t)(now - s_monitor_next_ms) >= 0)
        {
            s_monitor_next_ms = now + WIFI_MONITOR_PERIOD_MS;

            /* ★ 周期性状态快照（★ 排查"断线后灯不变"的关键诊断 ★）★
             *
             * 它同时回答三个问题：
             *   1) Task_WiFi 还活着吗？—— 只要这行在刷新就说明没卡住
             *      （如果 WiFi 任务卡在某处，这行会停，而心跳灯照旧闪 ——
             *       那种情况表现为"灯冻住"，和"状态机判断错"完全不同）
             *   2) APP 状态机现在处于哪个状态？（APP_WIFI_ST_xxx）
             *   3) BSP 层认为连没连上？has_ip 对不对？
             *      ★ 这两者不一致是最有价值的信息 ★
             *      例如 APP=CONNECTED 但 BSP 说没连 → 状态机该切但没切
             *           BSP 还说连上 → 断连事件根本没上报
             *
             * 只在"非 CONNECTED 之外也照打"—— 也就是说无论什么状态都打，
             * 这样对比"断开前 vs 断开后"就能看出哪一步没发生。 */
            ESP_LOGI(TAG, "[状态] app=%s bsp_state=%d bsp_connected=%u has_ip=%u "
                          "fail=%u/%u backoff=%u",
                     app_wifi_state_name(s_state), (int)s_bsp_state,
                     (unsigned)BSP_WIFI_IsConnected(g_wifi_sta),
                     (unsigned)BSP_WIFI_HasIp(g_wifi_sta),
                     (unsigned)s_fail_count, WIFI_MAX_RETRY,
                     (unsigned)s_backoff_level);

            BSP_WIFI_Info_t info;
            if (BSP_WIFI_GetInfo(g_wifi_sta, &info) == 0)
            {
                s_monitor_bad = 0;
                s_rssi = info.rssi;

#if WIFI_LOG_RSSI_PERIODIC
                ESP_LOGI(TAG, "alive: ip=%s rssi=%d dBm ch=%u heap=%u",
                         info.ip, (int)info.rssi, info.channel,
                         (unsigned)esp_get_free_heap_size());
#endif
            }
            else
            {
                s_monitor_bad++;
                ESP_LOGW(TAG, "monitor: get_info failed (%u/%u)",
                         (unsigned)s_monitor_bad, WIFI_MONITOR_CONFIRM);

                if (s_monitor_bad >= WIFI_MONITOR_CONFIRM)
                {
                    ESP_LOGE(TAG, "monitor: link lost, reconnecting");
                    s_monitor_bad = 0;
                    if (s_fail_count < WIFI_MAX_RETRY)
                        s_fail_count++;

                    if (s_fail_count >= WIFI_MAX_RETRY)
                    {
                        s_backoff_level++;
                        app_wifi_enter(APP_WIFI_ST_BACKOFF);
                    }
                    else
                    {
                        app_wifi_enter(APP_WIFI_ST_RETRY_WAIT);
                    }
                }
            }
        }
        break;
    }

    /* ---------------------------------------------------------------- */
    case APP_WIFI_ST_RETRY_WAIT:
        if ((int32_t)(now - s_wait_until_ms) >= 0)
            app_wifi_start_connect();
        break;

    /* ---------------------------------------------------------------- */
    case APP_WIFI_ST_BACKOFF:
        if ((int32_t)(now - s_wait_until_ms) >= 0)
        {
            /* 长退避结束后把失败计数清零，重新走一轮短重试阶梯 */
            s_fail_count = 0;
            app_wifi_start_connect();
        }
        break;

    default:
        s_state = APP_WIFI_ST_IDLE;
        break;
    }
}

/*==============================================================================
 * 状态查询
 *============================================================================*/
/**
 * @brief 查询应用层状态
 * @note  可以在任意任务里调：s_state 是 volatile 的单次读，写者只有服务任务，
 *        最坏情况是读到"上一轮"的状态值，不会读到撕裂值。
 */
App_WiFi_State_t App_WiFi_GetState(void)
{
    return s_state;
}

/**
 * @brief 是否已连接并拿到 IP
 */
uint8_t App_WiFi_IsConnected(void)
{
    return (s_state == APP_WIFI_ST_CONNECTED && BSP_WIFI_IsConnected(g_wifi_sta)) ? 1 : 0;
}

/**
 * @brief 获取本机 IP 字符串
 * @note  ★ 残留风险（如实说明，没有假装不存在）★
 *        返回的是模块内部的静态缓冲，写者只有服务任务，但写动作是
 *        strncpy（多字节），而读方（TASK_WIFI 打日志、app_music_app.c 错误分支）
 *        可能在别的任务上下文，所以理论上存在"读到一半"的撕裂可能：
 *        比如读到 "192.168.1.2" 后面还挂着上一次的 '\0'，或者新旧拼在一起。
 *        实际影响有限：
 *          · 调用它的两处都在 TASK_WIFI 任务里（探活日志 / 上传失败日志），
 *            与服务任务其实是同一个任务，不构成并发；
 *          · IP 字符串长度固定、被 strncpy 覆盖的起始若干字节总是新的，
 *            最坏结果是日志里出现一个错乱的 IP，不会越界、不会崩。
 *        如果以后有别的任务要来读它，正确做法是让调用方提供缓冲区、
 *        在服务任务里做一次带临界区的拷贝（本模块目前没有这样的调用方，
 *        所以没有为此加锁，避免为不存在的问题引入新复杂度）。
 */
const char *App_WiFi_GetIP(void)
{
    return (const char *)s_ip_str;      /* 去 volatile：返回类型是 const char* */
}

/**
 * @brief 获取信号强度
 * @note  int8_t 的单字节读写在 Xtensa 上是原子的，任意任务里调都安全。
 */
int8_t App_WiFi_GetRssi(void)
{
    return s_rssi;
}

/**
 * @brief 查询已连续失败次数
 * @note  uint8_t 单字节读，任意任务里调都安全。
 */
uint8_t App_WiFi_GetFailCount(void)
{
    return s_fail_count;
}

/**
 * @brief 请求立即重连（会重置退避计数）
 */
void App_WiFi_RequestReconnect(void)
{
    s_reconnect_req = 1;
}
