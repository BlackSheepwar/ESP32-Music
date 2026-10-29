/**
 * @file bsp_wifi.c
 * @brief WiFi BSP 层实现（句柄式接口，STA 客户端模式）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本文件把 ESP-IDF 的 WiFi 三件套（esp_netif / esp_wifi / event loop）
 *       收敛成「初始化 → 连接 → 查状态」三个动作，业务判断全部留给 APP 层。
 *
 *==============================================================================
 * ★ 关键设计取舍（踩过的坑，写在前面）★
 *==============================================================================
 *
 * 1) 关掉 ESP-IDF 的自动重连
 *    默认情况下 esp_wifi 断线后会自己反复重连，和 APP 层的「退避重连」叠加，
 *    表现为短时间内疯狂重连几十次、路由器把它拉黑（现象：之后再也连不上，
 *    必须重启路由器）。所以这里显式 esp_wifi_set_auto_connect(false)
 *    + 事件里不主动 connect，重连时机完全交给 APP 层。
 *
 * 2) 状态以「拿到 IP」为准，不是以「关联成功」为准
 *    WIFI_EVENT_STA_CONNECTED 只说明 802.11 关联上了；此时 DHCP 还没完成，
 *    直接发 HTTP 会失败（报 errno 118 / EHOSTUNREACH）。
 *    所以 CONNECTED 状态只在 IP_EVENT_STA_GOT_IP 里置位 —— 这是最容易踩的坑。
 *
 * 3) NVS 必须初始化（★ 原文档在这里写错了，实测被打回 ★）
 *    最初的注释写着「不用 NVS，跳过 nvs_flash_init() 省掉一个分区依赖」。
 *    这是错的：ESP-IDF 的 WiFi 驱动自己会无条件调 nvs_open() 存 PHY 校准数据，
 *    未初始化时 esp_wifi_init() 直接返回 ESP_ERR_NVS_NOT_INITIALIZED，
 *    WiFi 任务只能退出（现象：WiFi 指示灯永远不亮，只有生命灯在闪）。
 *    正确做法见 BSP_WIFI_Init() 的第 0 步。
 *
 *    esp_wifi_set_storage(WIFI_STORAGE_RAM) 仍然保留，但它只省掉
 *    「WiFi 连接配置」的 NVS 写入，省不掉驱动对 NVS 的依赖。
 *
 * 4) 状态变量用 volatile + 无锁读
 *    状态只在 WiFi 事件任务里写、在业务任务里读。单个 enum 的读写是原子的，
 *    所以不加互斥锁（加锁反而有在事件回调里阻塞的风险）。
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
#include "bsp_wifi.h"
// 功能包含
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"      /* ★ nvs_flash_init：WiFi 驱动依赖它，见 BSP_WIFI_Init ★ */
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "BSP_WIFI";

/*==============================================================================
 * 类型定义与硬件映射表
 *============================================================================*/
/**
 * @brief WiFi 句柄结构体定义（对上层不透明）
 */
struct BSP_WIFI_Handle {
    const char          *name;          // 日志名
    esp_netif_t         *netif;         // STA 网络接口
    BSP_WIFI_StateCb_t   state_cb;      // 状态回调
    volatile BSP_WIFI_State_t state;    // 当前状态
    volatile BSP_WIFI_Reason_t reason;  // 最近一次断连原因
    BSP_WIFI_Config_t    cfg;           // 当前连接配置（拷贝）
    uint8_t              inited;        // 是否已初始化
    uint8_t              has_ip;        // 是否已拿到 IP
    uint8_t              event_registered;// 事件处理器是否已注册
};

/**
 * @brief 句柄表（配置部分写死，运行时字段在 Init 里清零）
 */
static struct BSP_WIFI_Handle WIFI_RT_MAP[BSP_WIFI_MAX_NUM] = {
    {
        .name = "STA",
    },
};

const BSP_WIFI_Handle_t *const g_wifi_sta = &WIFI_RT_MAP[0];

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 更新状态并向 APP 层抛回调
 * @param h      运行时句柄
 * @param state  新状态
 * @param reason 断连原因
 * @note  只在状态真正变化时才回调，避免事件抖动导致 APP 层反复切灯。
 */
static void BSP_WIFI_Notify(struct BSP_WIFI_Handle *h, BSP_WIFI_State_t state, BSP_WIFI_Reason_t reason)
{
    BSP_WIFI_State_t old = h->state;

    h->state  = state;
    h->reason = reason;

    /* CONNECTED 之外的状态都清掉 has_ip，保证 IsConnected 不会误报 */
    if (state != BSP_WIFI_STATE_CONNECTED)
        h->has_ip = 0;

    if (h->state_cb != NULL && old != state)
    {
        /* ★ 回调在 WiFi 事件任务上下文里执行，栈很小，我们只传状态，不做别的 */
        h->state_cb((const BSP_WIFI_Handle_t *)h, state, reason);
    }
}

/**
 * @brief 把 ESP-IDF 的断连 reason 翻译成人话
 */
BSP_WIFI_Reason_t BSP_WIFI_TranslateReason(int esp_reason)
{
    switch (esp_reason)
    {
        case WIFI_REASON_AUTH_FAIL:                 /* 202 密码错 */
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:    /* 15  密码错 */
        case WIFI_REASON_HANDSHAKE_TIMEOUT:         /* 204 */
        case WIFI_REASON_MIC_FAILURE:               /* 14 */
        case WIFI_REASON_CONNECTION_FAIL:           /* 205 AP 拒绝 */
            return BSP_WIFI_REASON_AUTH_FAIL;

        case WIFI_REASON_NO_AP_FOUND:               /* 201 找不到该 WiFi */
            return BSP_WIFI_REASON_AP_NOT_FOUND;

        case WIFI_REASON_ASSOC_FAIL:                /* 203 */
            return BSP_WIFI_REASON_ASSOC_FAIL;

        case WIFI_REASON_BEACON_TIMEOUT:            /* 200 掉线 */
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:/* 4   被 AP 踢下线 */
            return BSP_WIFI_REASON_BEACON_TIMEOUT;

        default:
            return BSP_WIFI_REASON_OTHER;
    }
}

/**
 * @brief 原因枚举转字符串（日志用）
 */
static const char *BSP_WIFI_ReasonStr(BSP_WIFI_Reason_t reason)
{
    switch (reason)
    {
        case BSP_WIFI_REASON_AUTH_FAIL:         return "AUTH_FAIL(密码错/AP拒绝)";
        case BSP_WIFI_REASON_AP_NOT_FOUND:      return "AP_NOT_FOUND(找不到该WiFi)";
        case BSP_WIFI_REASON_ASSOC_FAIL:        return "ASSOC_FAIL(关联被拒)";
        case BSP_WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case BSP_WIFI_REASON_BEACON_TIMEOUT:    return "BEACON_TIMEOUT(掉线)";
        default:                                return "OTHER";
    }
}

/**
 * @brief 把 esp_netif 的 IP 信息填进 BSP_WIFI_Info_t
 */
static void BSP_WIFI_FillInfo(struct BSP_WIFI_Handle *h, BSP_WIFI_Info_t *info)
{
    esp_netif_ip_info_t ip_info = {0};
    memset(info, 0, sizeof(*info));

    if (esp_netif_get_ip_info(h->netif, &ip_info) == ESP_OK)
    {
        snprintf(info->ip,      sizeof(info->ip),      IPSTR, IP2STR(&ip_info.ip));
        snprintf(info->netmask, sizeof(info->netmask), IPSTR, IP2STR(&ip_info.netmask));
        snprintf(info->gateway, sizeof(info->gateway), IPSTR, IP2STR(&ip_info.gw));
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        memcpy(info->bssid, ap.bssid, sizeof(info->bssid));
        info->rssi    = ap.rssi;
        info->channel = ap.primary;
    }
}

/*==============================================================================
 * 事件处理
 *============================================================================*/
/**
 * @brief 统一事件回调
 * @param arg           注册时传进来的句柄
 * @param event_base    WIFI_EVENT / IP_EVENT
 * @param event_id      事件 ID
 * @param event_data    事件数据
 * @note  ★ 这里绝对不要做阻塞操作（vTaskDelay / 写 flash / 大段 printf），
 *          本任务的栈只有几 KB，栈溢出会以「莫名重启」的形式出现。
 */
static void BSP_WIFI_EventCb(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)arg;
    if (h == NULL)
        return;

    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGD(TAG, "%s: STA started", h->name);
            break;

        case WIFI_EVENT_STA_CONNECTED:
            /* 关联成功，但还没 IP：不做状态提升，只记日志 */
            ESP_LOGI(TAG, "%s: associated with AP, waiting for IP...", h->name);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
            int esp_reason = d ? d->reason : 0;
            BSP_WIFI_Reason_t reason = BSP_WIFI_TranslateReason(esp_reason);

            /* 之前连上过 → DISCONNECTED；从来没连上 → FAILED */
            BSP_WIFI_State_t next = (h->state == BSP_WIFI_STATE_CONNECTED)
                                    ? BSP_WIFI_STATE_DISCONNECTED
                                    : BSP_WIFI_STATE_FAILED;

            ESP_LOGW(TAG, "%s: disconnected, raw reason=%d (%s)",
                     h->name, esp_reason, BSP_WIFI_ReasonStr(reason));

            /* ★ 不在这里重连：重连时机由 APP 层控制，避免与退避策略打架 ★ */
            BSP_WIFI_Notify(h, next, reason);
            break;
        }

        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "%s: got IP " IPSTR, h->name, IP2STR(&e->ip_info.ip));

        /* ★ 只有拿到 IP 才算真正连上 ★ */
        h->has_ip = 1;
        BSP_WIFI_Notify(h, BSP_WIFI_STATE_CONNECTED, BSP_WIFI_REASON_UNKNOWN);
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief WiFi BSP 层初始化
 */
int BSP_WIFI_Init(void)
{
    struct BSP_WIFI_Handle *h = &WIFI_RT_MAP[0];

    if (h->inited)
        return 0;   /* 幂等 */

    /* ---------- 0. NVS 必须先初始化（★ 踩过的坑，不能省 ★）----------
     *
     * 最初这里没有 nvs_flash_init()，理由是"本工程把 WiFi 配置存在 RAM、
     * 不记密码，所以不需要 NVS"。这个推断是错的，实测现象：
     *     W (1021) wifi:wifi osi_nvs_open fail ret=4353
     *     E (1063) BSP_WIFI: esp_wifi_init failed: ESP_ERR_NVS_NOT_INITIALIZED
     *     E (1072) TASK_WIFI: App_WiFi_Init failed, task exit
     * 结果 WiFi 任务直接退出，WiFi 指示灯永远不亮。
     *
     * 根因：**ESP-IDF 的 WiFi 驱动自己会无条件调 nvs_open()**，
     * 用来存 PHY 射频校准数据和国家码 —— 这跟"要不要记住 WiFi 密码"无关。
     * 也就是说 esp_wifi_set_storage(WIFI_STORAGE_RAM) 只能省掉"WiFi 配置"的
     * NVS 写入，省不掉驱动对 NVS 本身的依赖。
     *
     * 分区表里 nvs 分区是存在的（24KB），只是从没被初始化过。
     * ★ 教训：凡是要用 WiFi / BLE / PHY 的工程，nvs_flash_init() 是必需的样板代码，
     *   不要因为"我不用 NVS 存业务数据"就跳过它。★ */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* NVS 分区满了，或版本不匹配（换 IDF 版本时会遇到）：擦掉重建。
         * 代价是丢掉已存的 PHY 校准数据，下次连接会重新校准。 */
        ESP_LOGW(TAG, "nvs_flash_init: %s, erasing and retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "NVS initialized (WiFi 驱动依赖它存 PHY 校准数据)");

    /* ---------- 1. 网络接口 ---------- */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* ---------- 2. 默认事件循环 ---------- */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "event_loop_create_default failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* ---------- 3. 建 STA 网络接口 ---------- */
    if (h->netif == NULL)
    {
        h->netif = esp_netif_create_default_wifi_sta();
        if (h->netif == NULL)
        {
            ESP_LOGE(TAG, "create_default_wifi_sta failed");
            return -1;
        }
    }

    /* ---------- 4. WiFi 驱动 ---------- */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* ---------- 5. 事件注册 ---------- */
    if (!h->event_registered)
    {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            BSP_WIFI_EventCb, h, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            BSP_WIFI_EventCb, h, NULL);
        h->event_registered = 1;
    }

    /* ---------- 6. WiFi 连接配置只存内存，不写 NVS ----------
     * 需求是「固定连一个 WiFi」，账号密码编译期就定死了，
     * 没必要把连接配置落盘。好处：
     *   1. 少一次 flash 写入，省寿命也省启动时间
     *   2. 不会因为 NVS 里存了旧 AP 而"连错路由器"（经典坑）
     * 代价：掉电不记忆连接信息 —— 对本工程无影响。
     *
     * ★ 注意区分：这【不】代表不需要初始化 NVS ★
     *   驱动本身仍要 NVS 存 PHY 校准数据，见本函数第 0 步。
     * ★ 必须在 esp_wifi_set_config() 之前设，否则配置已经落到 NVS 了。 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_mode STA failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* ★ 关键：不让 ESP-IDF 自己重连，重连统一由 APP_WIFI 的退避状态机调度 ★
     *
     * 为什么这里没有 esp_wifi_set_auto_connect(false)：
     *   该函数在 IDF 5.4 里没有对外的 setter（esp_wifi.h 未导出），
     *   公开手段只有下面两条，效果等价：
     *     1. wifi_config_t.sta.failure_retry_cnt = 0
     *        → 关联失败时驱动内部不重试（见 BSP_WIFI_Connect）
     *     2. 不开启 fast_connect / auto_connect 的默认配置
     *        → 掉线后驱动不主动重连，必须应用层再调 esp_wifi_connect()
     *   两个重连逻辑叠加会出现「疯狂重连」，进而被路由器拉黑，务必保持单一路径。
     */
    esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电：上传 160KB 音频时链路不能抖 */

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    h->state  = BSP_WIFI_STATE_IDLE;
    h->reason = BSP_WIFI_REASON_UNKNOWN;
    h->inited = 1;

    ESP_LOGI(TAG, "%s initialized (STA mode, auto-reconnect disabled, ps=none)", h->name);
    return 0;
}

/**
 * @brief 注册状态回调
 */
int BSP_WIFI_SetStateCb(const BSP_WIFI_Handle_t *handle, BSP_WIFI_StateCb_t cb)
{
    if (handle == NULL)
        return -1;

    ((struct BSP_WIFI_Handle *)handle)->state_cb = cb;
    return 0;
}

/*==============================================================================
 * 连接 / 断开
 *============================================================================*/
/**
 * @brief 启动连接（非阻塞）
 */
int BSP_WIFI_Connect(const BSP_WIFI_Handle_t *handle, const BSP_WIFI_Config_t *cfg)
{
    if (handle == NULL || cfg == NULL)
        return -1;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;

    if (!h->inited)
    {
        ESP_LOGE(TAG, "%s: not initialized, call BSP_WIFI_Init() first", h->name);
        return -1;
    }

    if (cfg->ssid[0] == '\0')
    {
        ESP_LOGE(TAG, "%s: SSID is empty", h->name);
        return -1;
    }

    /* 拷贝配置，调用方不必保持 cfg 存活 */
    memcpy(&h->cfg, cfg, sizeof(h->cfg));

    /* ---------- 静态 IP（可选） ---------- */
    if (cfg->static_ip_enable)
    {
        esp_netif_dhcpc_stop(h->netif);

        esp_netif_ip_info_t ip_info = {0};
        esp_netif_str_to_ip4(cfg->static_ip,      &ip_info.ip);
        esp_netif_str_to_ip4(cfg->static_netmask, &ip_info.netmask);
        esp_netif_str_to_ip4(cfg->static_gateway, &ip_info.gw);

        esp_err_t err = esp_netif_set_ip_info(h->netif, &ip_info);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "%s: set static ip failed: %s", h->name, esp_err_to_name(err));
            return -1;
        }
        ESP_LOGI(TAG, "%s: using static IP %s", h->name, cfg->static_ip);
    }
    else
    {
        esp_netif_dhcpc_start(h->netif);
    }

    /* ---------- Station 配置 ---------- */
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     cfg->ssid,     sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, cfg->password, sizeof(wcfg.sta.password) - 1);

    wcfg.sta.threshold.authmode = (cfg->password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wcfg.sta.channel             = cfg->channel;
    /* ★ scan_method 用 ALL_CHANNEL：不写死信道，换路由器也不用改代码 */
    wcfg.sta.scan_method         = WIFI_ALL_CHANNEL_SCAN;
    wcfg.sta.sort_method         = WIFI_CONNECT_AP_BY_SIGNAL;
    /* ★ 把 ESP-IDF 自身的重试关掉/压小，重试由 APP 层控制 */
    wcfg.sta.failure_retry_cnt   = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s: set_config failed: %s", h->name, esp_err_to_name(err));
        return -1;
    }

    /* ---------- 状态置为连接中并真正发起连接 ---------- */
    BSP_WIFI_Notify(h, BSP_WIFI_STATE_CONNECTING, BSP_WIFI_REASON_UNKNOWN);

    ESP_LOGI(TAG, "%s: connecting to \"%s\"...", h->name, cfg->ssid);

    /* ★ 若是从「已连接/已断开」状态重连，先 disconnect 一次保证干净 */
    esp_wifi_disconnect();

    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
        ESP_LOGE(TAG, "%s: esp_wifi_connect failed: %s", h->name, esp_err_to_name(err));
        BSP_WIFI_Notify(h, BSP_WIFI_STATE_FAILED, BSP_WIFI_REASON_OTHER);
        return -1;
    }

    return 0;
}

/**
 * @brief 主动断开
 */
int BSP_WIFI_Disconnect(const BSP_WIFI_Handle_t *handle)
{
    if (handle == NULL)
        return -1;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT)
    {
        ESP_LOGE(TAG, "%s: disconnect failed: %s", h->name, esp_err_to_name(err));
        return -1;
    }

    h->has_ip = 0;
    return 0;
}

/*==============================================================================
 * 状态查询
 *============================================================================*/
/**
 * @brief 查询当前状态
 */
BSP_WIFI_State_t BSP_WIFI_GetState(const BSP_WIFI_Handle_t *handle)
{
    if (handle == NULL)
        return BSP_WIFI_STATE_IDLE;

    return ((struct BSP_WIFI_Handle *)handle)->state;
}

/**
 * @brief 查询连接信息
 */
int BSP_WIFI_GetInfo(const BSP_WIFI_Handle_t *handle, BSP_WIFI_Info_t *info)
{
    if (handle == NULL || info == NULL)
        return -1;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;
    if (!h->has_ip)
        return -1;

    BSP_WIFI_FillInfo(h, info);
    return 0;
}

/**
 * @brief 查询是否已拿到 IP（诊断用）
 * @note  只返回 has_ip 这一位，不掺 state。
 *        诊断时把两者分开看，才能区分"IP 掉了"和"状态没更新"。
 */
uint8_t BSP_WIFI_HasIp(const BSP_WIFI_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    const struct BSP_WIFI_Handle *h = (const struct BSP_WIFI_Handle *)handle;
    return h->has_ip ? 1 : 0;
}

/**
 * @brief 查询是否已连接并拿到 IP
 */
uint8_t BSP_WIFI_IsConnected(const BSP_WIFI_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;
    return (h->state == BSP_WIFI_STATE_CONNECTED && h->has_ip) ? 1 : 0;
}

/*==============================================================================
 * DNS 覆盖（用来区分 DNS 污染 和 TCP 不可达）
 *============================================================================*/
int BSP_WIFI_SetDns(const BSP_WIFI_Handle_t *handle,
                    const char *primary, const char *secondary)
{
    if (handle == NULL || primary == NULL || primary[0] == '\0')
        return -1;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;

    if (!h->has_ip)
    {
        ESP_LOGW(TAG, "%s: 还没拿到 IP，现在设 DNS 会被丢弃（DHCP 会覆盖）", h->name);
        return -1;
    }

    esp_netif_dns_info_t dns = {0};

    /* esp_netif_set_dns_info 需要 esp_ip4_addr_t，用 esp_netif_str_to_ip4 解析 */
    if (esp_netif_str_to_ip4(primary, &dns.ip.u_addr.ip4) != ESP_OK)
    {
        ESP_LOGE(TAG, "DNS 地址格式错误: %s", primary);
        return -1;
    }
    dns.ip.type = ESP_IPADDR_TYPE_V4;

    esp_err_t err = esp_netif_set_dns_info(h->netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设置主 DNS 失败: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "%s: 主 DNS 已设为 %s", h->name, primary);

    if (secondary != NULL && secondary[0] != '\0')
    {
        memset(&dns, 0, sizeof(dns));
        if (esp_netif_str_to_ip4(secondary, &dns.ip.u_addr.ip4) == ESP_OK)
        {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            if (esp_netif_set_dns_info(h->netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK)
                ESP_LOGI(TAG, "%s: 备用 DNS 已设为 %s", h->name, secondary);
        }
    }

    return 0;
}

int BSP_WIFI_GetDns(const BSP_WIFI_Handle_t *handle, char *primary, char *secondary)
{
    if (handle == NULL || primary == NULL)
        return -1;

    struct BSP_WIFI_Handle *h = (struct BSP_WIFI_Handle *)handle;
    esp_netif_dns_info_t dns = {0};

    if (esp_netif_get_dns_info(h->netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4)
    {
        snprintf(primary, 16, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    else
    {
        snprintf(primary, 16, "(none)");
    }

    if (secondary != NULL)
    {
        memset(&dns, 0, sizeof(dns));
        if (esp_netif_get_dns_info(h->netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4)
        {
            snprintf(secondary, 16, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        else
        {
            snprintf(secondary, 16, "(none)");
        }
    }

    return 0;
}
