/**
 * @file bsp_wifi.h
 * @brief WiFi BSP 层（句柄式接口，STA 客户端模式）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块只做「把 WiFi 连上并告诉我状态」这件事，不含任何业务判断：
 *       - 上层用句柄启动连接：BSP_WIFI_Connect(g_wifi_sta, &cfg);
 *       - 状态变化通过回调抛给 APP 层（APP 层再决定 LED 怎么闪、要不要重连）
 *
 *       ★ 分层边界：BSP 层只封装 esp_wifi / esp_netif / nvs 的调用，
 *         不认识「LED」「音乐」「录音」；重连策略、指示灯语义全在 APP_WIFI 里。
 *
 *       ★ 连接凭据（SSID/密码）按需求写死在 APP_WIFI 的宏定义里，
 *         由 APP 层填进 BSP_WIFI_Config_t 传进来，BSP 层不持有宏。
 *
 *       ★ NVS 是必需的（原文档在这里写错过，实测被打回）★
 *         esp_wifi_init() 内部会无条件调 nvs_open() 存 PHY 射频校准数据和国家码，
 *         所以 BSP_WIFI_Init() 里必须先 nvs_flash_init()。
 *         这一点和「要不要记住 WiFi 密码」无关 ——
 *         本工程仍然把 WiFi 连接配置存在 RAM（esp_wifi_set_storage(WIFI_STORAGE_RAM)），
 *         只是省掉了配置落盘，省不掉驱动对 NVS 的依赖。
 *         跳过的现象：esp_wifi_init 返回 ESP_ERR_NVS_NOT_INITIALIZED，
 *         WiFi 任务退出，WiFi 指示灯永远不亮。
 */

#ifndef __BSP_WIFI_H__
#define __BSP_WIFI_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
#define BSP_WIFI_MAX_NUM        1       /* WiFi 实例最大数量（单 STA 场景用 1 个就够） */

#define BSP_WIFI_SSID_MAX_LEN   32      /* 802.11 SSID 上限 */
#define BSP_WIFI_PASS_MAX_LEN   64      /* WPA2 密码上限 */

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief WiFi 句柄（不透明类型，成员定义在 bsp_wifi.c）
 */
typedef struct BSP_WIFI_Handle BSP_WIFI_Handle_t;

/**
 * @brief WiFi 状态
 * @note  比 ESP-IDF 的 wifi_event_t 收敛过的状态集，够 APP 层做指示灯的语义判断：
 *        未初始化 → 空闲 → 连接中 → 已连接(有IP) → 断开 → 失败
 */
typedef enum {
    BSP_WIFI_STATE_IDLE = 0,        // 已初始化，尚未调用 Connect
    BSP_WIFI_STATE_CONNECTING,      // 正在关联 / 正在获取 IP
    BSP_WIFI_STATE_CONNECTED,       // 已连上且拿到 IP
    BSP_WIFI_STATE_DISCONNECTED,    // 曾经连上，现在掉线
    BSP_WIFI_STATE_FAILED,          // 连接尝试失败（密码错 / 找不到 AP）
} BSP_WIFI_State_t;

/**
 * @brief 断连原因（直接来自 ESP-IDF 的 wifi_err_reason_t，取常用的几个）
 */
typedef enum {
    BSP_WIFI_REASON_UNKNOWN          = 0,
    BSP_WIFI_REASON_AUTH_FAIL        = 1,   // 密码错（reason 15 / 205）
    BSP_WIFI_REASON_AP_NOT_FOUND     = 2,   // 找不到 AP（reason 201）
    BSP_WIFI_REASON_ASSOC_FAIL       = 3,   // 关联失败（reason 203）
    BSP_WIFI_REASON_HANDSHAKE_TIMEOUT= 4,   // 握手超时（reason 204）
    BSP_WIFI_REASON_BEACON_TIMEOUT   = 5,   // 掉线（reason 200）
    BSP_WIFI_REASON_OTHER            = 6,
} BSP_WIFI_Reason_t;

/**
 * @brief 状态变化回调
 * @param handle 句柄
 * @param state  新状态
 * @param reason 断连原因（仅在 DISCONNECTED / FAILED 时有意义）
 * @note  ★ 回调运行在 ESP-IDF 的 WiFi 事件任务上下文里（非中断，但栈很小，
 *          约 3.5KB），所以回调里【不要】做耗时操作、不要写 flash、
 *          不要 printf 大段日志。APP 层收到后应该只更新一个状态变量，
 *          真正的重连动作交给 TASK 层去轮询执行。
 */
typedef void (*BSP_WIFI_StateCb_t)(const BSP_WIFI_Handle_t *handle,
                                   BSP_WIFI_State_t state,
                                   BSP_WIFI_Reason_t reason);

/**
 * @brief WiFi 连接配置
 */
typedef struct {
    char     ssid[BSP_WIFI_SSID_MAX_LEN + 1];       // WiFi 名称（以 '\0' 结尾）
    char     password[BSP_WIFI_PASS_MAX_LEN + 1];   // WiFi 密码（开放网络填空串）
    uint8_t  channel;                               // 固定信道（0 = 全信道扫描）
    uint8_t  max_retry;                             // 单次 Connect 内 ESP-IDF 自身的重试次数
    uint16_t connect_timeout_ms;                    // 每次尝试的超时（毫秒）
    uint8_t  static_ip_enable;                      // 1 = 用静态 IP（见下）
    char     static_ip[16];                         // 静态 IP，如 "192.168.1.50"
    char     static_netmask[16];                    // 掩码，如 "255.255.255.0"
    char     static_gateway[16];                    // 网关，如 "192.168.1.1"
} BSP_WIFI_Config_t;

/**
 * @brief 连接结果信息（连上后可查）
 */
typedef struct {
    char     ip[16];        // 本机 IP
    char     netmask[16];   // 掩码
    char     gateway[16];   // 网关
    uint8_t  bssid[6];      // 接入点 MAC
    int8_t   rssi;          // 信号强度 dBm（-30 极好，-70 一般，-85 勉强）
    uint8_t  channel;       // 实际使用信道
} BSP_WIFI_Info_t;

/*==============================================================================
 * 具名句柄
 *============================================================================*/
/**
 * @brief 具名句柄：STA 客户端
 */
extern const BSP_WIFI_Handle_t *const g_wifi_sta;

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief WiFi BSP 层初始化
 * @retval 0: 成功
 * @retval -1: 失败（esp_netif / esp_wifi 初始化失败，串口有日志）
 * @note  做四件事：esp_netif_init → 建默认事件循环 → 建 STA netif → esp_wifi_init。
 *        不连接任何 AP，连哪个由 BSP_WIFI_Connect 决定。
 *        重复调用是幂等的。
 */
int BSP_WIFI_Init(void);

/**
 * @brief 注册状态回调
 * @param handle 句柄
 * @param cb     回调（NULL = 取消注册）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  必须在 BSP_WIFI_Connect 之前注册，否则会漏掉第一次的状态变化。
 */
int BSP_WIFI_SetStateCb(const BSP_WIFI_Handle_t *handle, BSP_WIFI_StateCb_t cb);

/*==============================================================================
 * 连接 / 断开
 *============================================================================*/
/**
 * @brief 启动连接（非阻塞）
 * @param handle 句柄（g_wifi_sta）
 * @param cfg    连接配置（函数内部会拷贝一份，调用方不必保持 cfg 存活）
 * @retval 0: 已下发连接命令
 * @retval -1: 参数错误或 esp_wifi_connect 失败
 * @note  ★ 非阻塞：函数立刻返回，连接结果通过状态回调通知。
 *        重复调用会先断开再重连，用于 APP 层的重连策略。
 */
int BSP_WIFI_Connect(const BSP_WIFI_Handle_t *handle, const BSP_WIFI_Config_t *cfg);

/**
 * @brief 主动断开（不会自动重连）
 * @param handle 句柄
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  断开后状态回调会走到 DISCONNECTED。ESP-IDF 默认会自动重连，
 *        本模块在事件处理里显式关掉了自动重连（见 .c），避免和 APP 层的
 *        重连退避策略打架（两个重连逻辑叠加会出现「疯狂重连」）。
 */
int BSP_WIFI_Disconnect(const BSP_WIFI_Handle_t *handle);

/*==============================================================================
 * 状态查询
 *============================================================================*/
/**
 * @brief 查询当前状态
 * @param handle 句柄
 * @retval 当前状态（句柄为 NULL 时返回 BSP_WIFI_STATE_IDLE）
 */
BSP_WIFI_State_t BSP_WIFI_GetState(const BSP_WIFI_Handle_t *handle);

/**
 * @brief 查询连接信息（IP / 信号强度等）
 * @param handle 句柄
 * @param info   输出信息
 * @retval 0: 成功（仅在 CONNECTED 状态下有意义）
 * @retval -1: 参数错误或尚未连接
 */
int BSP_WIFI_GetInfo(const BSP_WIFI_Handle_t *handle, BSP_WIFI_Info_t *info);

/**
 * @brief 查询是否已连接并拿到 IP
 * @param handle 句柄
 * @retval 1: 已连接
 * @retval 0: 未连接
 */
uint8_t BSP_WIFI_IsConnected(const BSP_WIFI_Handle_t *handle);

/**
 * @brief 覆盖 DNS 服务器（不用 DHCP 下发的）
 * @param handle    句柄
 * @param primary   主 DNS（点数格式，如 "223.5.5.5"）；传 NULL 或空串 = 不覆盖
 * @param secondary 备用 DNS（可为 NULL）
 * @retval 0: 成功
 * @retval -1: 参数错误或尚未获取到 IP（DNS 要在拿到 IP 之后才能设）
 * @note  ★ 用途：区分「DNS 污染」和「TCP 到不了」★
 *        默认用路由器下发的 DNS，如果它把这个域名解析到错误 IP，
 *        TCP 会立刻 EHOSTUNREACH，现象和"服务器挂了"一样。
 *        换成别的 DNS 一试就能把这两层分开。
 *        ★ 必须在拿到 IP 之后调用 ★（DHCP 完成前 netif 还没配好 IP，
 *          此时设 DNS 会被丢弃）。本工程在 GOT_IP 事件之后调。
 */
int BSP_WIFI_SetDns(const BSP_WIFI_Handle_t *handle,
                    const char *primary, const char *secondary);

/**
 * @brief 查询当前实际使用的 DNS 服务器（诊断用，确认覆盖有没有生效）
 * @param handle 句柄
 * @param primary   输出主 DNS（至少 16 字节）
 * @param secondary 输出备用 DNS（至少 16 字节，可为 NULL）
 * @retval 0: 成功
 * @retval -1: 失败
 */
int BSP_WIFI_GetDns(const BSP_WIFI_Handle_t *handle, char *primary, char *secondary);

/**
 * @brief 把断连 reason code 翻译成本模块的枚举（日志和 APP 层判断用）
 * @param esp_reason ESP-IDF 的 wifi_err_reason_t 原始值
 * @retval 归类后的原因
 */
BSP_WIFI_Reason_t BSP_WIFI_TranslateReason(int esp_reason);

/**
 * @brief 查询是否已拿到 IP（诊断用）
 * @param handle 句柄
 * @retval 1: 已拿到 IP
 * @retval 0: 没有 IP（含句柄为 NULL）
 *
 * @note  ★ 与 BSP_WIFI_IsConnected 的区别 ★
 *        `IsConnected = (state == CONNECTED && has_ip)` —— 业务判断用它。
 *        本函数只返回 `has_ip` 这一位，用于**诊断**：
 *        把它和 state 分开看，才能区分"IP 掉了"和"状态没更新"这两种故障。
 *        排查"WiFi 断了灯还常亮"时，这一位的价值就在这里。
 */
uint8_t BSP_WIFI_HasIp(const BSP_WIFI_Handle_t *handle);

#endif /* __BSP_WIFI_H__ */
