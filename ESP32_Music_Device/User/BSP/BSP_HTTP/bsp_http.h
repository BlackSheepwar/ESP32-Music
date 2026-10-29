/**
 * @file bsp_http.h
 * @brief HTTP/HTTPS 客户端 BSP 层（句柄式接口，对接后端 API）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 上层用句柄调用，不关心 esp_http_client 的细节：
 *
 *           BSP_HTTP_PostWav(g_http_api, wav, wav_len, NULL, 0);
 *
 *==============================================================================
 * ★ 这个文件里的三个坑，都是接口文档 API.md 明确点过的，别自己改回去 ★
 *==============================================================================
 *
 *  1) 超时必须是 30 秒，不能是默认的 5 秒
 *     esp_http_client 默认 timeout_ms = 5000。而真实链路耗时：
 *       上传 160KB      0.5~1.5s
 *       Whisper 推理    1~3s
 *       LLM 推理        1~3s
 *       ─────────────────────
 *       合计            3~8s
 *     症状特别误导人：串口报 timeout，但 wrangler tail 显示 Worker 成功返回 200，
 *     两边日志矛盾，极难排查。所以这里写死 BSP_HTTP_DEFAULT_TIMEOUT_MS = 30000。
 *
 *  2) 接收缓冲必须够大
 *     默认 RX buffer 只有 512 字节，而实际响应在 TOTAL_BARS=16 时约 6KB。
 *     所以本模块不用 esp_http_client 的内部缓冲，而是用 event handler
 *     把 HTTP_EVENT_ON_DATA 一片片收进调用方给的 buffer。
 *     调用方给 16KB 是安全的（32 小节时约 10KB）。
 *
 *  3) 证书用 esp_crt_bundle_attach，不要手动塞 pem
 *     Cloudflare 的证书在 ESP-IDF 内置的证书 bundle 里，自动挂上即可。
 *
 *==============================================================================
 * ★ 中国大陆网络现实 ★
 *==============================================================================
 *  Cloudflare 的 *.workers.dev 域名在中国大陆被墙（后端 wrangler.toml 的注释
 *  里也写了这一点）。所以本模块发请求必然失败，返回 BSP_HTTP_ERR_CONNECT。
 *  这是网络环境问题，不是固件 bug —— 等挂了自定义域名就会通。
 *  本工程用「后端指示灯」把这件事可视化出来（见 app_led_cfg.h）。
 */

#ifndef __BSP_HTTP_H__
#define __BSP_HTTP_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
#define BSP_HTTP_MAX_NUM            2       /* 支持几个 HTTP 实例（本工程用 1 个） */

/** @brief 建议的响应缓冲大小（字节）。32 小节时响应约 10KB，16KB 有充足余量 */
#define BSP_HTTP_RESP_BUF_SIZE      16384

/** @brief URL 最大长度 */
#define BSP_HTTP_URL_MAX_LEN        192

/** @brief Bearer token 最大长度（含结尾 '\0'） */
#define BSP_HTTP_TOKEN_MAX_LEN      64

/*==============================================================================
 * 重试策略（★ 针对"概率性 TLS 握手失败"★）
 *============================================================================*/
/**
 * @brief 单次请求最多尝试几次（含第一次）
 * @note  ★ 为什么必须重试 ★
 *        实测（2026-10，国内连 Cloudflare 自定义域名）：
 *          探活 #1 成功 (3919ms)
 *          探活 #2 成功 (3601ms)
 *          探活 #3 成功 (8073ms)
 *          探活 #4 失败：mbedtls_ssl_handshake returned -0x7280 (= SSL_TIMEOUT)
 *        失败原因是"握手包发出去但服务端响应被丢"，属于概率性干扰，
 *        不是确定性封锁。浏览器能稳定打开是因为它会自动重试多次，
 *        而本模块原本只试一次就放弃 —— 这就是"一会儿连上一会儿断联"的根因。
 *
 *        改成重试后，只要有一次握手穿过，请求就成功。
 *        实测成功率从约 75% 提升到接近 100%。
 */
/* ★ 从 3 提到 5（实测后调整）★
 *   实测：探活（262 字节）的握手要 3.6~13 秒；
 *   而上传 87404 字节 body 时会遇到"对端读到一半把连接重置"
 *   （mbedtls -0x0050 = NET_CONN_RESET，errno=Connection reset by peer），
 *   单次耗时可达 45 秒。这种失败是概率性的，多试几次能过 ——
 *   但每次失败都要 45 秒，3 次给的时间窗太窄，所以加到 5 次。
 *
 *   ★ 注意：这里提高次数对探活也没坏处 ★
 *     探活延迟高一点无所谓（它只是点个灯），而且拿不到锁会立刻跳过。 */
#define BSP_HTTP_MAX_RETRY          5

/**
 * @brief 每次重试前的等待基数（毫秒）
 * @note  ★ 从 800 提到 1000，并且改成【翻倍递增】：1s / 2s / 4s / 8s ★
 *
 *        实测教训（2026-10）：原来 800/1600/2400/3200ms 的间隔**太密**。
 *        链路劣化时单次尝试要熬 20~57 秒才失败，紧接着 0.8 秒就再建一条
 *        TLS 连接 —— 既没给网络恢复时间，也没给 mbedTLS 缓冲回收时间，
 *        结果把自己逼出 `errno=No more processes`（ENOMEM，本地资源耗尽）。
 *
 *        对"概率性丢包"这类失败，隔久一点再试的成功率明显更高，也不会堆资源。
 */
#define BSP_HTTP_RETRY_BASE_MS      1000

/** @brief 单次重试等待的上限（毫秒），防止翻倍递增过头 */
#define BSP_HTTP_RETRY_MAX_MS       8000

/**
 * @brief 单次尝试的超时（毫秒）
 * @note  ★ 从 30 秒降到 20 秒 ★
 *        30 秒是为"上传 160KB + Whisper + LLM 全链路"留的，那个场景确实需要。
 *        但现在失败的主要形态是 TLS 握手被丢包 —— 握手失败时
 *        干等 30 秒毫无意义（重试更快）。20 秒对正常请求仍有余量：
 *        实测成功请求的握手 3.6~8 秒，上传+推理另需几秒。
 */
#define BSP_HTTP_DEFAULT_TIMEOUT_MS 20000

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief HTTP 句柄（不透明类型，成员定义在 bsp_http.c）
 */
typedef struct BSP_HTTP_Handle BSP_HTTP_Handle_t;

/**
 * @brief 返回码
 */
typedef enum {
    BSP_HTTP_OK          =  0,      // 拿到 200 响应
    BSP_HTTP_ERR_PARAM   = -1,      // 参数错误
    BSP_HTTP_ERR_CONNECT = -2,      // 连不上（DNS 失败 / 被墙 / 无网）
    BSP_HTTP_ERR_TIMEOUT = -3,      // 超时（后端太慢，或链路太差）
    BSP_HTTP_ERR_STATUS  = -4,      // 连上了但 HTTP 状态码不是 200
    BSP_HTTP_ERR_BUF     = -5,      // 响应体超过调用方给的缓冲
    BSP_HTTP_ERR_NO_INIT = -6,      // 没有先调 Init
    /**
     * 上传被主动取消（新的上传请求顶掉了旧的）
     * @note  ★ 这不是失败，是"用户改主意了" ★
     *        见 BSP_HTTP_RequestCancel()。上层遇到它不应该报错、
     *        不应该点亮任何故障灯，最多打一行提示。
     */
    BSP_HTTP_ERR_ABORTED = -7,
    /**
     * 本地资源耗尽（lwIP 分配不出内存 / 缓冲 / PCB）
     * @note  ★ 这不是网络错误，而且【重试只会让它更糟】★
     *        实测（2026-10）：链路劣化时连续建立多条 TLS 连接
     *        （每条要 ~32KB mbedTLS 缓冲 + PCB + mbox），
     *        内部 RAM 最低掉到 147KB，随后报
     *        `esp_tls_conn_read error, errno=No more processes`
     *        —— 那就是 lwIP 的 ERR_MEM → ENOMEM(12) / ENOBUFS(105)。
     *        所以收到它必须立刻停止重试，否则重试本身在加重资源紧张。
     */
    BSP_HTTP_ERR_NOMEM = -8,
} BSP_HTTP_Ret_t;

/**
 * @brief 响应信息
 */
typedef struct {
    int      status_code;               // HTTP 状态码
    int      body_len;                  // 实际收到的 body 字节数（不含结尾 '\0'）
    uint8_t  truncated;                 // 1 = body 超过缓冲被截断
    uint32_t elapsed_ms;                // 本次请求耗时（毫秒）
} BSP_HTTP_Resp_t;

/*==============================================================================
 * 具名句柄
 *============================================================================*/
/**
 * @brief 具名句柄：对接音乐意图后端
 */
extern const BSP_HTTP_Handle_t *const g_http_api;

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief 初始化 BSP HTTP 层
 * @retval 0: 成功
 * @retval -1: 失败
 * @note  本层不需要 WiFi 已经连上就能初始化（它只是准备客户端配置）。
 *        但发请求前必须确认网络已就绪，见 App_WiFi_IsConnected()。
 *        重复调用是幂等的。
 */
int BSP_HTTP_Init(void);

/**
 * @brief 设置后端基址（不含路径），例如 "https://music-intent-api.xxx.workers.dev"
 * @param handle 句柄
 * @param base_url 基址
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  ★ 必须 https:// 开头。用 http:// 会连不上（后端只有 https）。
 */
int BSP_HTTP_SetBaseUrl(const BSP_HTTP_Handle_t *handle, const char *base_url);

/**
 * @brief 设置 Bearer token（后端 DEVICE_TOKEN）
 * @param handle 句柄
 * @param token  token 字符串；传空串或 NULL = 不带 Authorization 头
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  后端 wrangler.toml 里 DEVICE_TOKEN = ""，即当前不校验，所以默认不带头。
 *        以后开了鉴权，把 token 填进 app_music_cfg.h 的宏即可。
 */
int BSP_HTTP_SetToken(const BSP_HTTP_Handle_t *handle, const char *token);

/*==============================================================================
 * 请求
 *============================================================================*/
/**
 * @brief POST 原始 WAV 字节到后端意图接口
 * @param handle      句柄
 * @param wav         WAV 数据（含 44 字节头）
 * @param wav_len     数据长度
 * @param resp_buf    响应缓冲（建议 BSP_HTTP_RESP_BUF_SIZE）
 * @param resp_cap    缓冲容量
 * @param resp        输出响应信息（可为 NULL）
 * @retval BSP_HTTP_OK: 成功（HTTP 200）
 * @retval 其它负值: 失败，见 BSP_HTTP_Ret_t
 * @note  请求路径固定为 {base_url}/api/v1/intent，会自动带 Content-Type: audio/wav。
 *        路径上会附加 ?pretty=0 让后端返回紧凑 JSON（体积减半，省 TLS 时间和缓冲）。
 *        失败时串口会打印后端返回的 error 字段，便于定位。
 */
BSP_HTTP_Ret_t BSP_HTTP_PostWav(const BSP_HTTP_Handle_t *handle,
                                const uint8_t *wav, size_t wav_len,
                                char *resp_buf, size_t resp_cap,
                                BSP_HTTP_Resp_t *resp);

/**
 * @brief GET 任意路径（用于 /api/v1/health 探活）
 * @param handle   句柄
 * @param path     以 '/' 开头的路径，如 "/api/v1/health"
 * @param resp_buf 响应缓冲
 * @param resp_cap 缓冲容量
 * @param resp     输出响应信息（可为 NULL）
 * @retval BSP_HTTP_OK: 成功
 * @retval 其它负值: 失败
 * @note  ★ 这个接口用来做「是否连接后端」的指示灯判断 ★
 *        比发一个空 POST 干净得多：health 不调 AI、不耗额度、响应极小。
 */
BSP_HTTP_Ret_t BSP_HTTP_Get(const BSP_HTTP_Handle_t *handle, const char *path,
                            char *resp_buf, size_t resp_cap,
                            BSP_HTTP_Resp_t *resp);

/**
 * @brief 查询基址是否已配置（非空且不是占位符）
 * @param handle 句柄
 * @retval 1: 已配置
 * @retval 0: 未配置
 * @note  未配置时后端指示灯应当熄灭，而不是一直闪 ——
 *        否则用户会以为「在连」，实际是根本没填地址。
 */
uint8_t BSP_HTTP_IsConfigured(const BSP_HTTP_Handle_t *handle);

/*==============================================================================
 * 取消机制（★ 用于"新的上传请求顶掉旧的"★）
 *============================================================================*/
/**
 * @brief 请求取消当前（或下一次）HTTP 请求，并清掉未处理的取消请求
 *
 * @note  ★ 能取消到什么程度（实测过 ESP-IDF 5.4.4 的实现，必须如实说明）★
 *
 *        `esp_http_client_perform()` 内部对 body 的发送是
 *        `esp_http_client_write(client, post_data + written, left)` ——
 *        **一次 write 调用内部没有任何回调点**，所以：
 *
 *          ❌ **正在传输中的那一次 write 无法中断**。
 *             它可能阻塞几十秒（对端不收数据时 TCP 发送缓冲填满），
 *             这段时间本函数帮不上忙。
 *
 *          ✅ 但下面两个时机能立刻生效：
 *             1) **重试之间** —— 每次都检查，取消后不再重试
 *             2) **下一个请求开始时** —— 请求前会消费掉取消标志
 *
 *        ★ 所以语义是这样 ★
 *          「取消 = 不再开始新的尝试」，而不是「立刻掐断当前 TCP 连接」。
 *          对一个"最多 5 次重试、每次最坏 45 秒"的链路来说，
 *          这个语义已经足够：一旦你按下 KEY1 录第二段，
 *          第一次上传最迟在这次尝试结束后就会退出，不会继续重试 4 次。
 *
 *        ★ 为什么不做成"真的中断连接" ★
 *          那需要在事件回调里调 esp_http_client_close()，而那是在
 *          perform 的执行栈里 —— 关闭自己正在用的 client 属于未定义行为。
 *          用"不再重试"这种协作式取消是安全且可预期的。
 *
 * @param handle 句柄
 * @param cancel 1 = 请求取消；0 = 清除取消标志（每次新上传开始前调一次）
 */
void BSP_HTTP_RequestCancel(const BSP_HTTP_Handle_t *handle, uint8_t cancel);

/**
 * @brief 返回码转字符串（日志用）
 */
const char *BSP_HTTP_RetStr(BSP_HTTP_Ret_t ret);

/*==============================================================================
 * 网络分层诊断（★ 用来把"连不上"拆成三个层次 ★）
 *============================================================================*/
/**
 * @brief 对后端域名做一次纯 TCP 连接测试（不走 TLS / HTTP）
 * @param handle    句柄
 * @param host      主机名（如 "music-intent-api.xxx.workers.dev"）
 * @param port      端口（https = 443）
 * @param timeout_ms 超时
 * @param out_ip    输出：解析到的 IP（至少 16 字节，可为 NULL）
 * @retval  0: TCP 连接成功（说明 DNS 和路由都没问题，问题在 TLS 层）
 * @retval -1: 参数错误
 * @retval -2: DNS 解析失败（域名根本解析不出来）
 * @retval -3: DNS 解析到了地址，但 TCP 连不通（★ 最可能的情况：IP 被污染 / 被墙）
 *
 * @note  ★ 为什么要有这个函数 ★
 *        只报"HTTP 连接失败"是没法排查的：可能是域名解析错、可能解析对了但
 *        路由到不了、也可能 TCP 通了但 TLS 握手被中间设备打断。
 *        这个函数只做 DNS + TCP connect，把前两层单独测出来：
 *          -2 → DNS 有问题（换 DNS 可能解决）
 *          -3 → DNS 给了地址但连不上（IP 被污染，或直连路径被掐）
 *           0 → 前两层都好，问题在 TLS/SNI 层
 *        三种结论对应的修法完全不同，所以值得单列一个函数。
 *
 *        ★ 用 lwip 的原始 socket（不是 esp_http_client）★
 *          因为要的就是"不涉及应用层协议"，用 HTTP 客户端测会和被诊断的对象混在一起。
 */
int BSP_HTTP_DiagTcp(const BSP_HTTP_Handle_t *handle, const char *host, int port,
                     uint32_t timeout_ms, char *out_ip);

/**
 * @brief 从 base_url 里抠出主机名
 * @param handle 句柄
 * @param out    输出缓冲（至少 128 字节）
 * @param cap    容量
 * @retval 0: 成功
 * @retval -1: 失败（URL 格式不对）
 * @note  只支持 "https://host/..." 或 "http://host/..." 形式。
 */
int BSP_HTTP_GetHost(const BSP_HTTP_Handle_t *handle, char *out, size_t cap);

#endif /* __BSP_HTTP_H__ */
