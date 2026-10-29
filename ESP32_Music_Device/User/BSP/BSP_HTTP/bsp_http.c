/**
 * @file bsp_http.c
 * @brief HTTP/HTTPS 客户端 BSP 层实现
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本层把 esp_http_client 的「配置 + 事件累积 + 错误归类」收成一个函数调用。
 *
 *==============================================================================
 * ★ 实现要点（都是踩过的坑，改代码前先读）★
 *==============================================================================
 *
 *  1) 响应体用 event handler 累加，不依赖内部 RX 缓冲
 *     esp_http_client 的内部 RX buffer 默认只有 512 字节，而真实响应约 6KB。
 *     所以配 cfg.buffer_size = 512（够放响应头），body 靠 HTTP_EVENT_ON_DATA
 *     回调一片片抄进调用方的 buffer。这样缓冲大小完全由调用方控制。
 *
 *  2) 二进制 body 用 esp_http_client_set_post_field，不要用字符串拼接
 *     WAV 里有 0x00 字节，任何 str* 函数都会在第一个 0 截断。set_post_field
 *     接收「指针 + 长度」，是唯一正确的发送方式。
 *
 *  3) 错误要分类，不能都叫 "failed"
 *     连不上（被墙/DNS 失败）和超时（后端慢）是完全不同的问题，
 *     处置方式也不同。所以这里读 esp_http_client_get_errno() 做归类。
 *
 *  4) 非 200 时把后端返回的 body 打出来
 *     后端在 400/401/413/502 时都会在 body 里带 {ok:false, error, hint}，
 *     其中 hint 直接告诉你该改什么。固件端把它打到串口，等于把云端的诊断信息
 *     搬到了设备串口上，省掉一轮「连电脑看日志」。
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
#include "bsp_http.h"
#include "bsp_sys.h"        /* BSP_SYS_GetTickMs */
// 功能包含
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// 网络分层诊断（BSP_HTTP_DiagTcp）用的原始 socket
/* 网络分层诊断（BSP_HTTP_DiagTcp）的原始 socket。
 * ★ 地址格式化不借用任何宏 ★
 *   esp_netif 的 IP2STR 期望 esp_ip4_addr_t（成员叫 addr），
 *   而这里从 getaddrinfo 拿到的是 sockaddr_in（sin_addr 成员叫 s_addr），
 *   两者类型不同，混用会报 "struct in_addr has no member named addr"。
 *   直接按字节手工格式化，零依赖、也不受 include 顺序影响。 */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "BSP_HTTP";

/**
 * @brief 取消标志（volatile：写侧来自别的任务，读侧在本模块的请求线程里）
 * @note  语义与限制见 bsp_http.h 里 BSP_HTTP_RequestCancel() 的说明。
 *        它是**协作式取消**：只保证不再开始新的尝试，不保证掐断正在传输的连接。
 */
static volatile uint8_t s_cancel_req = 0;

void BSP_HTTP_RequestCancel(const BSP_HTTP_Handle_t *handle, uint8_t cancel)
{
    (void)handle;
    s_cancel_req = cancel ? 1 : 0;

    if (cancel)
        ESP_LOGW(TAG, "已请求取消上传（最迟在当前这次尝试结束后生效）");
}


/** @brief 内部 RX 缓冲：只用来放响应头，body 走 event 回调 */
#define BSP_HTTP_INTERNAL_RX_BUF    512

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 响应累积器
 * @note  通过 esp_http_client_config_t.user_data 传给事件回调。
 *        只在单次请求期间存活（栈上变量），所以不需要互斥保护 ——
 *        本工程的所有 HTTP 请求都从同一个任务发出。
 */
typedef struct {
    char    *buf;           // 调用方给的缓冲
    size_t   cap;           // 容量
    size_t   len;           // 已收到多少
    uint8_t  truncated;     // 是否发生过截断
} bsp_http_acc_t;

/* 前向声明：bsp_http_request（带重试的包装器）会调用 bsp_http_request_once，
 * 而后者的定义在包装器之后 —— 所以这里必须先声明一次。 */
static BSP_HTTP_Ret_t bsp_http_request_once(const struct BSP_HTTP_Handle *h,
                                            const char *path,
                                            const uint8_t *post_data, size_t post_len,
                                            char *resp_buf, size_t resp_cap,
                                            BSP_HTTP_Resp_t *resp);

/**
 * @brief HTTP 句柄结构体定义（对上层不透明）
 */
struct BSP_HTTP_Handle {
    char     base_url[BSP_HTTP_URL_MAX_LEN];    // 后端基址（不含路径）
    char     token[BSP_HTTP_TOKEN_MAX_LEN];     // Bearer token
    uint8_t  inited;
};

/*==============================================================================
 * 具名句柄
 *============================================================================*/
static struct BSP_HTTP_Handle HTTP_RT_MAP[BSP_HTTP_MAX_NUM];

const BSP_HTTP_Handle_t *const g_http_api = &HTTP_RT_MAP[0];

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 事件回调：累积响应体
 */
static esp_err_t bsp_http_event_cb(esp_http_client_event_t *e)
{
    bsp_http_acc_t *acc = (bsp_http_acc_t *)e->user_data;

    switch (e->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (acc == NULL || acc->buf == NULL || e->data == NULL || e->data_len <= 0)
            break;

        /* 留一个字节给结尾 '\0'（方便上层直接当字符串用） */
        if (acc->len + (size_t)e->data_len < acc->cap)
        {
            memcpy(acc->buf + acc->len, e->data, (size_t)e->data_len);
            acc->len += (size_t)e->data_len;
        }
        else
        {
            /* 能塞多少塞多少，然后标记截断 —— 上层据此判断响应被切了 */
            size_t room = (acc->cap > acc->len + 1) ? (acc->cap - acc->len - 1) : 0;
            if (room > 0)
            {
                memcpy(acc->buf + acc->len, e->data, room);
                acc->len += room;
            }
            acc->truncated = 1;
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        if (acc != NULL && acc->buf != NULL && acc->cap > 0)
            acc->buf[acc->len < acc->cap ? acc->len : (acc->cap - 1)] = '\0';
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "http event error");
        break;

    default:
        break;
    }

    return ESP_OK;
}

/**
 * @brief 把 esp 的错误码归类成 BSP_HTTP_Ret_t
 */
static BSP_HTTP_Ret_t bsp_http_classify_errno(int errno_val)
{
    switch (errno_val)
    {
    /* ---------- 连不上：DNS 失败、被墙、路由不可达 ---------- */
    case ECONNRESET:        /* 104 对端重置（被墙时很常见） */
    case EHOSTUNREACH:      /* 118 lwIP ERR_RTE */
    case ENETUNREACH:       /* 114 */
    /* ★ 下面三个刻意保留裸数字，不是疏漏 ★
     *   113 = Linux 的 EHOSTUNREACH（ESP32 newlib 用的是 118）。
     *         ESP-IDF 有些路径会把两套错误码混着返回，所以两个值都要接。
     *   202 = EAI_FAIL、203 = EAI_NONAME —— 这两个来自 lwIP 的
     *         <lwip/netdb.h>（getaddrinfo 的返回值体系），
     *         它们**不是** errno，所以没有对应的 errno 宏可用。 */
    case 113:
    case 202:
    case 203:
        return BSP_HTTP_ERR_CONNECT;

    case ETIMEDOUT:         /* 116 */
        return BSP_HTTP_ERR_TIMEOUT;

    /* ---------- ★ 本地资源耗尽：单独识别，绝不重试 ★ ----------
     * lwIP 的 ERR_MEM / ERR_BUF → 分配不出 PCB / mbox / 缓冲。
     * 实测症状：`esp_tls_conn_read error, errno=No more processes`
     *           —— 那个 errno 数值就是 ENOMEM(12)。
     * 这时候再建连接只会让资源更紧张，所以归类成专用错误码，
     * 由重试循环直接放弃（见 bsp_http_request）。
     *
     * ★ 注意：这里用符号常量而不是裸数字 ★
     *   原实现写的是 `case 104:` `case 116:` `case 118:`。
     *   用 ESP32 工具链预处理器核对过，它们在 newlib/lwIP 下确实
     *   等于 ECONNRESET / ETIMEDOUT / EHOSTUNREACH，所以原来是对的；
     *   但写成符号常量后，换平台或换 newlib 版本都不会悄悄失效。 */
    case ENOMEM:            /* 12  lwIP ERR_MEM */
    case ENOBUFS:           /* 105 lwIP ERR_BUF */
    case EMFILE:            /* 24  socket（文件描述符）用尽 */
    case ENFILE:
        return BSP_HTTP_ERR_NOMEM;

    default:
        return BSP_HTTP_ERR_CONNECT;
    }
}

/**
 * @brief 组装完整 URL 到静态缓冲
 * @param h    句柄
 * @param path 以 '/' 开头的路径
 * @param out  输出缓冲
 * @param cap  输出缓冲容量
 * @retval 0: 成功
 * @retval -1: 缓冲不够或参数错误
 */
static int bsp_http_build_url(const struct BSP_HTTP_Handle *h, const char *path,
                              char *out, size_t cap)
{
    if (h == NULL || path == NULL || out == NULL)
        return -1;

    int n = snprintf(out, cap, "%s%s", h->base_url, path);
    if (n < 0 || (size_t)n >= cap)
    {
        ESP_LOGE(TAG, "url too long: %s%s", h->base_url, path);
        return -1;
    }
    return 0;
}

/**
 * @brief 通用请求
 * @param h         句柄
 * @param path      路径（以 '/' 开头）
 * @param post_data POST 数据（NULL = GET）
 * @param post_len  POST 长度
 * @param resp_buf  响应缓冲
 * @param resp_cap  容量
 * @param resp      输出响应信息（可 NULL）
 */
/**
 * @brief 带重试的请求入口
 * @note  ★ 为什么加这一层 ★
 *        实测国内连 Cloudflare 自定义域名时，TLS 握手是【概率性】成功的：
 *          探活 #1 成功 3919ms / #2 成功 3601ms / #3 成功 8073ms
 *          #4 失败 mbedtls_ssl_handshake returned -0x7280 (= SSL_TIMEOUT)
 *        失败形态是"握手包发出去了、服务端响应被丢"，不是确定性拒绝。
 *        浏览器能稳定打开是因为它会自动重试多次；固件只试一次就会表现为
 *        "一会儿连上一会儿断联"。
 *
 *        重试策略：
 *          · 只在【网络层失败】时重试（CONNECT / TIMEOUT）
 *            —— HTTP 4xx/5xx 是后端明确的回答，重试没意义，直接返回
 *          · 退避递增（800ms / 1600ms），因为干扰是成片的，
 *            隔一会儿才有空窗期能穿过去
 *          · 每次重试前重置响应缓冲，避免上一次的残留拼到这一次后面
 *
 *        ★ 注意：重试只发生在"连接握手"阶段，不会重复上传音频 ★
 *          因为失败时请求体根本没发出去（TLS 没建起来）。如果以后要处理
 *          "已上传但响应超时"的情况，需要后端幂等支持，那是另一回事。
 */
static BSP_HTTP_Ret_t bsp_http_request(const struct BSP_HTTP_Handle *h,
                                       const char *path,
                                       const uint8_t *post_data, size_t post_len,
                                       char *resp_buf, size_t resp_cap,
                                       BSP_HTTP_Resp_t *resp)
{
    BSP_HTTP_Ret_t ret = BSP_HTTP_ERR_CONNECT;

    for (int attempt = 0; attempt < BSP_HTTP_MAX_RETRY; attempt++)
    {
        /* ★ 取消检查（协作式）★
         *
         * 放在每次尝试的【开头】，一旦取消，后面的重试全部跳过。
         *
         * ★ 这里做不到什么（如实说明）★
         *   正在传输中的那一次 write 无法中断 —— esp_http_client_perform
         *   内部是 `esp_http_client_write(client, post_data + written, left)`，
         *   一次 write 调用内部没有回调点。它可能阻塞几十秒
         *   （对端不收数据时 TCP 发送缓冲填满）。
         *   所以"取消"的语义是「不再开始新的尝试」，不是「掐断当前连接」，
         *   详见 bsp_http.h 里 BSP_HTTP_RequestCancel() 的说明。
         *
         * ★ 不清取消标志 ★
         *   由调用方在"开始一次新上传"时显式清（见 app_music_app.c）。
         *   如果在这里清，就没法表达"取消一个还没开始的请求"了。 */
        if (s_cancel_req)
        {
            ESP_LOGW(TAG, "上传已取消：跳过第 %d 次尝试（共 %d 次）",
                     attempt + 1, BSP_HTTP_MAX_RETRY);
            return BSP_HTTP_ERR_ABORTED;
        }

        /* 每次重试都清一次响应缓冲，避免上一次的残留混进这一次 */
        if (resp_buf != NULL && resp_cap > 0)
            resp_buf[0] = '\0';

        ret = bsp_http_request_once(h, path, post_data, post_len,
                                   resp_buf, resp_cap, resp);

        if (ret == BSP_HTTP_OK)
        {
            if (attempt > 0)
                ESP_LOGI(TAG, "重试第 %d 次成功", attempt);
            return ret;
        }

        /* ---------- ★ 本地资源耗尽：立刻放弃，绝不重试 ★ ----------
         *
         * 实测（2026-10）：链路劣化时，一次上传会连续失败 5 次，
         * 每次都建一条新 TLS 连接（~32KB mbedTLS 缓冲 + PCB + mbox），
         * 内部 RAM 最低掉到 147KB，随后开始报：
         *     esp_tls_conn_read error, errno=No more processes   ← ENOMEM
         *     HTTP_CLIENT: Incomplete data received, ret=0, 1970/2664 bytes
         *
         * 这时候【重试是在加重问题】—— 每多试一次就再抢一份资源，
         * 而 mbedTLS 的缓冲要等连接彻底回收才释放。
         * 所以立刻返回，把"再试"的决定权交给上层（隔一段时间再来）。 */
        if (ret == BSP_HTTP_ERR_NOMEM)
        {
            ESP_LOGE(TAG, "本地资源耗尽（lwIP 分配不出内存/缓冲），立即停止重试");
            ESP_LOGE(TAG, "  这不是网络问题 —— 连续建 TLS 连接把内部 RAM 用光了");
            ESP_LOGE(TAG, "  内部 RAM 现在只剩 %u 字节",
                     (unsigned)esp_get_free_heap_size());
            ESP_LOGE(TAG, "  ↳ 等十几秒让连接彻底回收，再按 KEY1 重试");
            return ret;
        }

        /* 只在网络层失败时重试；后端明确答复（4xx/5xx）不重试 */
        if (ret != BSP_HTTP_ERR_CONNECT && ret != BSP_HTTP_ERR_TIMEOUT)
        {
            ESP_LOGW(TAG, "非网络层错误(%s)，不重试", BSP_HTTP_RetStr(ret));
            return ret;
        }

        if (attempt + 1 < BSP_HTTP_MAX_RETRY)
        {
            /* ★ 退避加大：从 800ms 线性递增改成 1s 起步、翻倍递增 ★
             *
             * 实测教训：原来 800 / 1600 / 2400 / 3200 ms 的间隔太密。
             * 链路劣化时每次尝试要熬 20~57 秒才失败，紧接着 0.8 秒就再建一条
             * TLS 连接 —— 既没给网络恢复时间，也没给 mbedTLS 缓冲回收时间，
             * 结果把自己逼进 ENOMEM。
             *
             * 现在是 1s / 2s / 4s / 8s（翻倍）。对"概率性丢包"这种失败，
             * 隔久一点再试的成功率明显更高，而且不会堆资源。 */
            uint32_t wait = BSP_HTTP_RETRY_BASE_MS << attempt;
            if (wait > BSP_HTTP_RETRY_MAX_MS)
                wait = BSP_HTTP_RETRY_MAX_MS;

            ESP_LOGW(TAG, "第 %d/%d 次尝试失败(%s)，%u ms 后重试...",
                     attempt + 1, BSP_HTTP_MAX_RETRY, BSP_HTTP_RetStr(ret),
                     (unsigned)wait);
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
        else
        {
            ESP_LOGE(TAG, "已重试 %d 次仍失败，放弃。", BSP_HTTP_MAX_RETRY);
            ESP_LOGE(TAG, "  若失败形态是 mbedtls_ssl_handshake -0x7280(SSL_TIMEOUT)，");
            ESP_LOGE(TAG, "  说明 TLS 握手被概率性丢包 —— 这是网络环境问题，");
            ESP_LOGE(TAG, "  多试几次通常能过；持续失败则考虑换网络或换后端托管。");
        }
    }

    return ret;
}


static BSP_HTTP_Ret_t bsp_http_request_once(const struct BSP_HTTP_Handle *h,
                                       const char *path,
                                       const uint8_t *post_data, size_t post_len,
                                       char *resp_buf, size_t resp_cap,
                                       BSP_HTTP_Resp_t *resp)
{
    if (h == NULL || !h->inited)
    {
        ESP_LOGE(TAG, "not initialized");
        return BSP_HTTP_ERR_NO_INIT;
    }
    if (h->base_url[0] == '\0')
    {
        ESP_LOGE(TAG, "base url not set");
        return BSP_HTTP_ERR_PARAM;
    }
    if (resp_buf == NULL || resp_cap < 64)
    {
        ESP_LOGE(TAG, "response buffer too small");
        return BSP_HTTP_ERR_PARAM;
    }

    static char url[BSP_HTTP_URL_MAX_LEN + 64];
    if (bsp_http_build_url(h, path, url, sizeof(url)) != 0)
        return BSP_HTTP_ERR_PARAM;

    /* 响应累积器：栈上变量，生命周期覆盖整个请求 */
    bsp_http_acc_t acc = {
        .buf       = resp_buf,
        .cap       = resp_cap,
        .len       = 0,
        .truncated = 0,
    };
    resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = bsp_http_event_cb,
        .user_data         = &acc,
        .method            = (post_data != NULL) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        /* ★ 30 秒，不是默认的 5 秒。理由见 bsp_http.h 文件头 ★ */
        .timeout_ms        = BSP_HTTP_DEFAULT_TIMEOUT_MS,
        /* ★ Cloudflare 证书在 IDF 内置 bundle 里，自动挂载 ★ */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* 内部缓冲只放响应头，body 走回调 */
        .buffer_size       = BSP_HTTP_INTERNAL_RX_BUF,
        .buffer_size_tx    = 1024,
        .keep_alive_enable = false,     /* 一次性请求，不复用连接，省状态 */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "client init failed (out of memory?)");
        return BSP_HTTP_ERR_CONNECT;
    }

    /* ---------- 请求头 ---------- */
    /* ★ 二进制 body 必须走 set_post_field（指针＋长度），不能用字符串拼接 ★ */
    if (post_data != NULL)
    {
        esp_http_client_set_header(client, "Content-Type", "audio/wav");
        esp_http_client_set_post_field(client, (const char *)post_data, (int)post_len);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (h->token[0] != '\0')
    {
        static char auth[BSP_HTTP_TOKEN_MAX_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", h->token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    /* ---------- 执行 ---------- */
    uint32_t t0 = BSP_SYS_GetTickMs();
    ESP_LOGI(TAG, "%s %s (%u bytes upload)",
             (post_data != NULL) ? "POST" : "GET", url, (unsigned)post_len);

    esp_err_t err = esp_http_client_perform(client);
    uint32_t elapsed = BSP_SYS_GetTickMs() - t0;

    int status = esp_http_client_get_status_code(client);
    int errno_val = esp_http_client_get_errno(client);

    BSP_HTTP_Ret_t ret;

    if (err != ESP_OK)
    {
        ret = bsp_http_classify_errno(errno_val);
        ESP_LOGE(TAG, "request failed: %s (errno=%d, %u ms) -> %s",
                 esp_err_to_name(err), errno_val, (unsigned)elapsed, BSP_HTTP_RetStr(ret));
        if (ret == BSP_HTTP_ERR_CONNECT)
        {
            ESP_LOGE(TAG, "  ↳ 连不上后端。当前配置的是自定义域名（www.230508.xyz），");
            ESP_LOGE(TAG, "    若这里失败，按下面的分层诊断结论处理；");
            ESP_LOGE(TAG, "    若用的是 *.workers.dev，那在国内必然失败（DNS 被投毒），换域名即可。");
        }
    }
    else if (status != 200)
    {
        ret = BSP_HTTP_ERR_STATUS;
        ESP_LOGE(TAG, "HTTP %d (errno=%d, %u ms)", status, errno_val, (unsigned)elapsed);
        /* ★ 后端在非 200 时也会带完整诊断，把它打出来 ★ */
        if (acc.len > 0)
            ESP_LOGE(TAG, "  body: %s", resp_buf);
    }
    else
    {
        ret = BSP_HTTP_OK;
        ESP_LOGI(TAG, "HTTP 200 OK, body %u bytes%s, %u ms",
                 (unsigned)acc.len, acc.truncated ? " (TRUNCATED!)" : "",
                 (unsigned)elapsed);

        if (acc.truncated)
        {
            ret = BSP_HTTP_ERR_BUF;
            ESP_LOGE(TAG, "  ↳ 响应被截断！把接收缓冲调大（当前 %u 字节，"
                          "32 小节响应约 10KB，建议 %u）",
                     (unsigned)resp_cap, (unsigned)BSP_HTTP_RESP_BUF_SIZE);
        }
    }

    if (resp != NULL)
    {
        resp->status_code = status;
        resp->body_len    = (int)acc.len;
        resp->truncated   = acc.truncated;
        resp->elapsed_ms  = elapsed;
    }

    esp_http_client_cleanup(client);
    return ret;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
int BSP_HTTP_Init(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(HTTP_RT_MAP); i++)
    {
        if (!HTTP_RT_MAP[i].inited)
        {
            memset(&HTTP_RT_MAP[i], 0, sizeof(HTTP_RT_MAP[i]));
            HTTP_RT_MAP[i].inited = 1;
        }
    }

    ESP_LOGI(TAG, "init done (timeout = %d ms, crt bundle attached)",
             BSP_HTTP_DEFAULT_TIMEOUT_MS);
    return 0;
}

int BSP_HTTP_SetBaseUrl(const BSP_HTTP_Handle_t *handle, const char *base_url)
{
    if (handle == NULL || base_url == NULL)
        return -1;

    struct BSP_HTTP_Handle *h = (struct BSP_HTTP_Handle *)handle;

    if (strlen(base_url) >= sizeof(h->base_url))
    {
        ESP_LOGE(TAG, "base url too long (max %u)", (unsigned)sizeof(h->base_url) - 1);
        return -1;
    }

    strncpy(h->base_url, base_url, sizeof(h->base_url) - 1);
    h->base_url[sizeof(h->base_url) - 1] = '\0';

    /* 去掉可能多写的结尾 '/'，避免拼出 "//api/v1/intent" */
    size_t n = strlen(h->base_url);
    while (n > 0 && h->base_url[n - 1] == '/')
        h->base_url[--n] = '\0';

    ESP_LOGI(TAG, "base url = %s", h->base_url);
    return 0;
}

int BSP_HTTP_SetToken(const BSP_HTTP_Handle_t *handle, const char *token)
{
    if (handle == NULL)
        return -1;

    struct BSP_HTTP_Handle *h = (struct BSP_HTTP_Handle *)handle;

    if (token == NULL || token[0] == '\0')
    {
        h->token[0] = '\0';
        ESP_LOGI(TAG, "token cleared (no Authorization header)");
        return 0;
    }

    if (strlen(token) >= sizeof(h->token))
    {
        ESP_LOGE(TAG, "token too long (max %u)", (unsigned)sizeof(h->token) - 1);
        return -1;
    }

    strncpy(h->token, token, sizeof(h->token) - 1);
    h->token[sizeof(h->token) - 1] = '\0';
    ESP_LOGI(TAG, "token set (%u chars)", (unsigned)strlen(h->token));
    return 0;
}

/*==============================================================================
 * 请求
 *============================================================================*/
BSP_HTTP_Ret_t BSP_HTTP_PostWav(const BSP_HTTP_Handle_t *handle,
                                const uint8_t *wav, size_t wav_len,
                                char *resp_buf, size_t resp_cap,
                                BSP_HTTP_Resp_t *resp)
{
    if (wav == NULL || wav_len == 0)
        return BSP_HTTP_ERR_PARAM;

    /* ★ ?pretty=0：让后端返回紧凑 JSON。
     *   API.md 实测：16 小节美化输出约 6.2KB，紧凑输出约 2.6KB。
     *   体积减半意味着 TLS 传输时间减半，接收缓冲也能小一半。★ */
    return bsp_http_request((const struct BSP_HTTP_Handle *)handle,
                            "/api/v1/intent?pretty=0",
                            wav, wav_len, resp_buf, resp_cap, resp);
}

BSP_HTTP_Ret_t BSP_HTTP_Get(const BSP_HTTP_Handle_t *handle, const char *path,
                            char *resp_buf, size_t resp_cap,
                            BSP_HTTP_Resp_t *resp)
{
    if (path == NULL || path[0] != '/')
        return BSP_HTTP_ERR_PARAM;

    return bsp_http_request((const struct BSP_HTTP_Handle *)handle,
                            path, NULL, 0, resp_buf, resp_cap, resp);
}

uint8_t BSP_HTTP_IsConfigured(const BSP_HTTP_Handle_t *handle)
{
    if (handle == NULL)
        return 0;

    const struct BSP_HTTP_Handle *h = (const struct BSP_HTTP_Handle *)handle;

    if (h->base_url[0] == '\0')
        return 0;

    /* 占位符也算没配置 —— 否则后端灯会一直闪，用户以为在连，其实没填地址 */
    if (strstr(h->base_url, "YOUR_") != NULL)
        return 0;

    return 1;
}

/*==============================================================================
 * 网络分层诊断
 *============================================================================*/
int BSP_HTTP_GetHost(const BSP_HTTP_Handle_t *handle, char *out, size_t cap)
{
    if (handle == NULL || out == NULL || cap < 8)
        return -1;

    const struct BSP_HTTP_Handle *h = (const struct BSP_HTTP_Handle *)handle;

    const char *p = strstr(h->base_url, "://");
    if (p == NULL)
        return -1;
    p += 3;

    size_t n = 0;
    while (p[n] != '\0' && p[n] != '/' && p[n] != ':' && n < cap - 1)
    {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';

    return (n > 0) ? 0 : -1;
}

int BSP_HTTP_DiagTcp(const BSP_HTTP_Handle_t *handle, const char *host, int port,
                     uint32_t timeout_ms, char *out_ip)
{
    (void)handle;

    if (host == NULL || port <= 0)
        return -1;

    if (out_ip != NULL)
        out_ip[0] = '\0';

    /* ---------- 1. DNS 解析 ---------- */
    struct addrinfo hints = {0};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int gerr = getaddrinfo(host, port_str, &hints, &res);
    if (gerr != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "[诊断] DNS 解析失败: %s (getaddrinfo=%d)", host, gerr);
        ESP_LOGE(TAG, "       -> 域名解析不出来。换一个 DNS 服务器可能解决。");
        if (res) freeaddrinfo(res);
        return -2;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    char ip[16];
    /* 手工格式化 IPv4（s_addr 是网络字节序，最低字节是第一个八位组） */
    uint32_t a32 = addr->sin_addr.s_addr;
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)(a32 & 0xFF), (unsigned)((a32 >> 8) & 0xFF),
             (unsigned)((a32 >> 16) & 0xFF), (unsigned)((a32 >> 24) & 0xFF));
    if (out_ip != NULL)
        snprintf(out_ip, 16, "%s", ip);

    ESP_LOGW(TAG, "[诊断] %s 解析为 %s", host, ip);

    /* ---------- 2. TCP 连接（非阻塞 + select 控超时） ---------- */
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "[诊断] socket 创建失败");
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret != 0 && errno != EINPROGRESS)
    {
        ESP_LOGE(TAG, "[诊断] TCP %s:%d 连接失败: errno=%d", ip, port, errno);
        close(sock);
        return -3;
    }

    if (ret != 0)
    {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);

        struct timeval sel_tv;
        sel_tv.tv_sec  = (time_t)(timeout_ms / 1000);
        sel_tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);

        int sel = select(sock + 1, NULL, &wset, NULL, &sel_tv);

        if (sel <= 0)
        {
            ESP_LOGE(TAG, "[诊断] TCP %s:%d 超时（%u ms）", ip, port, (unsigned)timeout_ms);
            ESP_LOGE(TAG, "       -> DNS 给了地址但连不上。典型原因：");
            ESP_LOGE(TAG, "          a) 解析到的是被污染的假 IP（最常见）");
            ESP_LOGE(TAG, "          b) 该 IP 到本网络的路径被掐");
            ESP_LOGE(TAG, "       -> 先试换 DNS；再不行只能用自定义域名或换网络。");
            close(sock);
            return -3;
        }

        int so_err = 0;
        socklen_t len = sizeof(so_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (so_err != 0)
        {
            ESP_LOGE(TAG, "[诊断] TCP %s:%d 连接失败: errno=%d", ip, port, so_err);
            close(sock);
            return -3;
        }
    }

    ESP_LOGI(TAG, "[诊断] TCP %s:%d 连接成功", ip, port);
    ESP_LOGI(TAG, "       -> DNS 和路由都正常，问题在 TLS/握手层或服务端拒绝");
    close(sock);
    return 0;
}
const char *BSP_HTTP_RetStr(BSP_HTTP_Ret_t ret)
{
    switch (ret)
    {
    case BSP_HTTP_OK:          return "OK";
    case BSP_HTTP_ERR_PARAM:   return "PARAM";
    case BSP_HTTP_ERR_CONNECT: return "CONNECT_FAIL(DNS/TCP/TLS 握手失败)";
    case BSP_HTTP_ERR_TIMEOUT: return "TIMEOUT";
    case BSP_HTTP_ERR_STATUS:  return "HTTP_STATUS_NOT_200";
    case BSP_HTTP_ERR_BUF:     return "RESP_TRUNCATED";
    case BSP_HTTP_ERR_NO_INIT: return "NOT_INIT";
    case BSP_HTTP_ERR_ABORTED: return "ABORTED(被新请求取消)";
    case BSP_HTTP_ERR_NOMEM:   return "LOCAL_NOMEM(本地资源耗尽，不是网络问题)";
    default:                   return "UNKNOWN";
    }
}
