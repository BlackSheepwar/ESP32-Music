/**
 * @file app_music_app.c
 * @brief 音乐业务应用层实现
 * @author 李嘉图
 * @date 2026-06-26
 *
 *==============================================================================
 * ★ 内存布局（都放 PSRAM，理由见下）★
 *==============================================================================
 *  本模块要两块大内存：
 *    响应缓冲  16KB    —— 后端在 TOTAL_BARS=16 时约 6KB，32 小节约 10KB
 *    music_t   约 1.5KB —— 数组为主
 *  另外 KS 合成器实例约 66KB（32 声部 × 512 float）。
 *
 *  为什么不放内部 RAM：
 *    内部 RAM 只有 512KB，其中 WiFi 协议栈要 ~50KB、TLS 握手要 ~40KB、
 *    录音缓冲 256KB（已在 PSRAM）、各任务栈 ~25KB。
 *    再把 66KB 的合成器和 16KB 的响应缓冲塞进去，很容易在
 *    「连上 WiFi 又发起 HTTPS」的那一刻分配失败 ——
 *    而且失败点往往在 mbedtls 内部，报错信息完全看不出是内存不够。
 *  所以：
 *    music_t + 响应缓冲 + 合成器实例  全部 PSRAM
 *    KS 声部的延迟线（float）留在合成器结构体内部，也就跟着进了 PSRAM。
 *    ★ 代价是 PSRAM 访问比内部 RAM 慢（约 2~4 倍延迟）★
 *      但 KS 合成的内层循环是顺序访问、命中的是 cache line，
 *      实测压力远小于「每样点随机跳转」的算法。若日后发现爆音，
 *      把 KS_MAX_VOICES 调小或把声部延迟线改成内部 RAM 分配即可。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ds_common.h"
#include "app_music_app.h"
#include "app_music_cfg.h"
// 功能包含
#include "app_audio.h"
#include "app_audio_cfg.h"
#include "app_wifi.h"
#include "bsp_http.h"
#include "bsp_i2s.h"        /* BSP_I2S_SAMPLE_RATE / BSP_I2S_WAV_HEADER_SIZE */
#include "ks_synth.h"
#include "cJSON.h"          /* 从响应里单独抠 text / intent_source */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 静态状态
 *============================================================================*/
static const char *TAG = "APP_MUSIC";

/** @brief 后端响应缓冲（PSRAM） */
static char *s_resp_buf = NULL;

/** @brief 解析后的音乐数据（PSRAM） */
static music_t *s_music = NULL;

/** @brief KS 合成器实例（PSRAM，约 66KB） */
static ks_synth_t *s_synth = NULL;

/** @brief 是否已成功准备过音乐 */
static volatile uint8_t s_has_audio = 0;

/** @brief 最近一次识别的文本 */
static char s_last_text[160] = {0};

/*==============================================================================
 * ★ HTTP 访问串行化（审计修复）★
 *============================================================================*/
/**
 * @brief 保护「后端响应缓冲」和 HTTP 层静态资源的互斥量
 *
 * @note  ★ 为什么需要它 ★
 *        本模块的探活和上传由【两个不同任务】发起：
 *          · Task_WiFi_Service  (core 0, prio 4) → App_MusicApp_ProbeBackend()
 *          · Task_Key_Cmd      (core 1, prio 5) → App_MusicApp_UploadAndPrepare()
 *        两者共用同一块 `s_resp_buf`，而 bsp_http.c 内部还有 **static** 的
 *        URL 缓冲和 Authorization 缓冲（见 bsp_http.c 的 `static char url[...]`
 *        和 `static char auth[...]`）。这些 static 缓冲被所有句柄共享。
 *
 *        并发调用的后果（都实测分析过）：
 *          1) 响应串台：探活的 health JSON 和上传的 music JSON 交叉写进同一块缓冲，
 *             cJSON 解析到两块拼接的内容 → 解析失败或解析出乱七八糟的参数
 *          2) URL 串台：一个任务正在用 url 缓冲做请求，另一个把它改成别的路径
 *          3) 更隐蔽的：探活每 30 秒一次、上传是一次性的，撞上的概率不低
 *
 *        修法：在 APP 层用一把互斥量把「访问后端」这个动作整体串行化。
 *        ★ 用互斥量而不是自旋锁 ★
 *          因为临界区里会阻塞几秒到几十秒（TLS 握手、上传、等 AI）——
 *          自旋锁会在这期间把另一个核心的 CPU 全烧掉，必须用会让出 CPU 的互斥量。
 *
 *        ★ 代价与取舍 ★
 *          探活若正好撞上上传，会等上传完成（最多约 60 秒）才继续。
 *          这可以接受：探活只是点亮指示灯，晚一点无所谓；
 *          反过来让探活插进上传中间去改 static url 才是真正的错误。
 *
 *        ★ 为什么不在 BSP_HTTP 层加锁 ★
 *          那样每个句柄各自一把锁，仍然挡不住 static 缓冲被跨句柄共享的问题；
 *          而且要等所有子代理报告汇总后再决定。这里先在 APP 层做对，
 *          并在 bsp_http.h 里把这个限制记录清楚（见那边的说明）。
 */
static SemaphoreHandle_t s_http_mux = NULL;

/**
 * @brief 获取/释放后端访问权
 * @retval 0: 拿到
 * @retval -1: 没初始化或超时
 * @note  超时给得比较长（70 秒），因为上传全链路最坏要 60 秒左右。
 *        宁可等，也不要在没拿到锁的情况下动那块共享缓冲。
 */
static int backend_lock(void)
{
    if (s_http_mux == NULL)
        return -1;
    return (xSemaphoreTake(s_http_mux, pdMS_TO_TICKS(70000)) == pdTRUE) ? 0 : -1;
}

/**
 * @brief 尝试获取后端访问权，拿不到就立刻返回（不等待）
 * @retval 0: 拿到
 * @retval -1: 别人正在用 / 没初始化
 *
 * @note  ★ 为什么探活要用这个而不是 backend_lock（审计修复 M9）★
 *
 *        探活全链路最坏是「3 次重试 × 20 秒超时 + 退避」≈ 62 秒。
 *        如果探活用 70 秒超时去等锁，而它正好撞上上传，那么：
 *          · task_key_cmd 会被卡住最多 70 秒 → **按键全部无响应**
 *            （探活每 30 秒一次，所以这个概率并不低）
 *          · 反过来探活自己等不到锁时会返回 -1，
 *            task_wifi 会据此把后端灯设成"不可达"→ **后端明明可达却报假故障**
 *
 *        探活是"尽力而为"的低优先级动作：拿不到锁就跳过，等下一个 30 秒周期。
 *        让路的应该是探活，不是用户的按键。
 */
static int backend_trylock(void)
{
    if (s_http_mux == NULL)
        return -1;
    return (xSemaphoreTake(s_http_mux, 0) == pdTRUE) ? 0 : -1;
}

static void backend_unlock(void)
{
    if (s_http_mux != NULL)
        xSemaphoreGive(s_http_mux);
}

/*==============================================================================
 * 初始化
 *============================================================================*/
int App_MusicApp_Init(void)
{
    /* ---------- 1. HTTP 层 ---------- */
    if (BSP_HTTP_Init() != 0)
    {
        ESP_LOGE(TAG, "BSP_HTTP_Init failed");
        return -1;
    }

    if (BSP_HTTP_SetBaseUrl(g_http_api, BACKEND_BASE_URL) != 0)
    {
        ESP_LOGE(TAG, "set backend url failed (too long?)");
        return -1;
    }

    /* token 为空就不带 Authorization 头（后端 DEVICE_TOKEN 现在就是空的） */
    BSP_HTTP_SetToken(g_http_api, BACKEND_DEVICE_TOKEN);

    /* ---------- 2. 配置检查 ---------- */
    if (!BSP_HTTP_IsConfigured(g_http_api))
    {
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, " BACKEND_BASE_URL is still the placeholder!");
        ESP_LOGE(TAG, " Edit User/APP/APP_MUSIC/app_music_cfg.h:");
        ESP_LOGE(TAG, "   #define BACKEND_BASE_URL \"https://www.230508.xyz\"");
        ESP_LOGE(TAG, " 本工程用的是自定义域名（www.230508.xyz），国内直连实测可用。");
        ESP_LOGE(TAG, " 换域名：Cloudflare → Workers & Pages → music-intent-api");
        ESP_LOGE(TAG, "        → Settings → Domains & Routes → Add → Custom Domain");
        ESP_LOGE(TAG, "==================================================");
    }
    else
    {
        ESP_LOGI(TAG, "backend configured: %s", BACKEND_BASE_URL);
        ESP_LOGI(TAG, "  upload: POST %s/api/v1/intent?pretty=0", BACKEND_BASE_URL);
        ESP_LOGI(TAG, "  probe : GET  %s/api/v1/health", BACKEND_BASE_URL);
    }

    /* ---------- 3. 大内存（PSRAM 优先，内部 RAM 兜底） ---------- */
    /* 先建互斥量：必须早于任何一次后端访问（探活任务可能已经跑起来了） */
    if (s_http_mux == NULL)
    {
        s_http_mux = xSemaphoreCreateMutex();
        if (s_http_mux == NULL)
        {
            ESP_LOGE(TAG, "create http mutex failed");
            return -1;
        }
    }

    s_resp_buf = (char *)heap_caps_malloc(MUSIC_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_music    = (music_t *)heap_caps_malloc(sizeof(music_t), MALLOC_CAP_SPIRAM);
    s_synth    = (ks_synth_t *)heap_caps_malloc(sizeof(ks_synth_t), MALLOC_CAP_SPIRAM);

    /* 兜底：PSRAM 没起来时退回内部 RAM（会紧，但至少能跑） */
    if (s_resp_buf == NULL)
        s_resp_buf = (char *)heap_caps_malloc(MUSIC_RESP_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_music == NULL)
        s_music = (music_t *)heap_caps_malloc(sizeof(music_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_synth == NULL)
        s_synth = (ks_synth_t *)heap_caps_malloc(sizeof(ks_synth_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_resp_buf == NULL || s_music == NULL || s_synth == NULL)
    {
        ESP_LOGE(TAG, "alloc failed: resp=%p music=%p synth=%p",
                 s_resp_buf, s_music, s_synth);
        ESP_LOGE(TAG, "  heap: internal %u B, PSRAM %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return -1;
    }

    ESP_LOGI(TAG, "init done: resp buf %u B, music %u B, synth %u KB",
             (unsigned)MUSIC_RESP_BUF_SIZE,
             (unsigned)sizeof(music_t),
             (unsigned)(sizeof(ks_synth_t) / 1024));
    ESP_LOGI(TAG, "  heap after alloc: internal %u B, PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    return 0;
}

/*==============================================================================
 * 后端探活
 *============================================================================*/
int App_MusicApp_ProbeBackend(void)
{
    if (s_resp_buf == NULL)
        return -1;

    if (!BSP_HTTP_IsConfigured(g_http_api))
        return -1;

    /* ★ 探活用不等待的方式拿锁（审计修复 M9）★
     * 拿不到就跳过这一轮：不阻塞按键任务，也不要把灯设成"不可达"
     * （后端可能好好的，只是正在被上传占用）。 */
    if (backend_trylock() != 0)
    {
        ESP_LOGD(TAG, "探活跳过：后端正被上传占用（下一周期再试）");
        return APP_MUSIC_ERR_BUSY_SKIP;
    }

    BSP_HTTP_Resp_t resp;
    BSP_HTTP_Ret_t ret = BSP_HTTP_Get(g_http_api, "/api/v1/health",
                                      s_resp_buf, 1024, &resp);

    if (ret == BSP_HTTP_OK)
    {
        /* health 返回很小，直接打出来便于确认后端阶段（mock/echo/asr/full） */
        ESP_LOGI(TAG, "backend OK (%u ms): %s",
                 (unsigned)resp.elapsed_ms, s_resp_buf);
    }
    else
    {
        ESP_LOGW(TAG, "backend probe failed: %s", BSP_HTTP_RetStr(ret));
    }

    /* ★ 必须在读 s_resp_buf 的日志打完之后才释放锁 ★
     *   （上面的 ESP_LOGI 直接引用了 s_resp_buf 的内容） */
    backend_unlock();

    return (ret == BSP_HTTP_OK) ? 0 : -1;
}

uint8_t App_MusicApp_IsBackendConfigured(void)
{
    return BSP_HTTP_IsConfigured(g_http_api);
}

/*==============================================================================
 * WAV 缓冲
 *============================================================================*/
/**
 * @brief WAV 打包缓冲
 * @note  ★ 放 PSRAM ★
 *        最长录音 8 秒 → WAV = 44 + 256000 ≈ 256KB。
 *        如果放内部 RAM，会和 WiFi/TLS 抢内存；而且这个缓冲只在
 *        「录音结束 → 上传」这一段活着，放 PSRAM 完全不影响性能
 *        （HTTP 上传本来就慢）。
 */
static uint8_t *s_wav_buf = NULL;
static size_t   s_wav_cap = 0;

/**
 * @brief 惰性分配 WAV 缓冲
 * @retval 0: 成功
 * @retval -1: 失败
 */
static int music_ensure_wav_buf(void)
{
    if (s_wav_buf != NULL)
        return 0;

    /* 44 字节头 + 最长录音（8 秒 @16kHz = 256000 字节） */
    s_wav_cap = BSP_I2S_WAV_HEADER_SIZE
              + (size_t)BSP_I2S_SAMPLE_RATE * AUDIO_REC_MAX_SECONDS * sizeof(int16_t);

    s_wav_buf = (uint8_t *)heap_caps_malloc(s_wav_cap, MALLOC_CAP_SPIRAM);
    if (s_wav_buf == NULL)
        s_wav_buf = (uint8_t *)heap_caps_malloc(s_wav_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_wav_buf == NULL)
    {
        ESP_LOGE(TAG, "wav buffer alloc failed (%u bytes)", (unsigned)s_wav_cap);
        return -1;
    }

    ESP_LOGI(TAG, "wav buffer allocated: %u bytes", (unsigned)s_wav_cap);
    return 0;
}

/*==============================================================================
 * 主流程
 *============================================================================*/
App_Music_Result_t App_MusicApp_UploadAndPrepare(void)
{
    /* ---------- 0. 前置检查 ---------- */
    /* ★ 清掉上一次留下的"取消"意图 ★
     *
     * 语义：取消是"针对某一次上传"的意图，不是常驻开关。
     * 现在的调用时序保证它是安全的：
     *   · 用户按 KEY1 → App_Audio_RecordStart() 会置取消标志
     *     （意在顶掉正在跑的那一次上传）
     *   · 用户松手 → 本函数被调用 → 在这里把标志清掉，
     *     于是【本次上传不会被自己误取消】
     * 所以清标志的位置就在上传入口，必须在打包/上传之前。 */
    BSP_HTTP_RequestCancel(g_http_api, 0);

    if (!App_WiFi_IsConnected())
    {
        ESP_LOGE(TAG, "upload aborted: WiFi not connected (ip=%s)", App_WiFi_GetIP());
        return APP_MUSIC_ERR_WIFI;
    }

    if (!BSP_HTTP_IsConfigured(g_http_api))
    {
        ESP_LOGE(TAG, "upload aborted: backend url not configured");
        return APP_MUSIC_ERR_NO_URL;
    }

    size_t rec_frames = App_Audio_GetRecordFrames(AUDIO_REC_MAIN);
    if (rec_frames == 0)
    {
        ESP_LOGE(TAG, "upload aborted: no recorded audio");
        return APP_MUSIC_ERR_NO_AUDIO;
    }

    /* ★ 从这里开始独占后端访问权 ★
     * 覆盖：打包 WAV → 上传 → 解析 → 初始化合成器。
     * 全程持锁，避免探活任务插进来把 s_resp_buf 改写掉。 */
    if (backend_lock() != 0)
    {
        ESP_LOGE(TAG, "upload aborted: 另一个后端请求正在进行（等锁超时）");
        return APP_MUSIC_ERR_HTTP;
    }

    /* ★ 受锁区用 do{...}while(0) 包起来，所有错误路径改走 break ★
     * 这样只有一个出口，解锁不会漏 —— 原来的写法有 5 个 return，
     * 每个都要记得解锁，迟早漏一个（漏了就是永久死锁，探活和上传全停）。 */
    App_Music_Result_t result = APP_MUSIC_ERR_HTTP;

    do
    {
        /* ---------- 1. 打包 WAV ---------- */
        if (music_ensure_wav_buf() != 0)
            break;

        int wav_len = App_Audio_BuildWav(AUDIO_REC_MAIN, s_wav_buf, s_wav_cap);
        if (wav_len <= 0)
        {
            ESP_LOGE(TAG, "build wav failed (%d)", wav_len);
            result = APP_MUSIC_ERR_NO_AUDIO;
            break;
        }

        ESP_LOGI(TAG, "uploading %d bytes (%.1f s of audio)...",
                 wav_len, (double)rec_frames / (double)BSP_I2S_SAMPLE_RATE);

        /* ---------- 2. 上传 ---------- */
        BSP_HTTP_Resp_t resp;
        BSP_HTTP_Ret_t ret = BSP_HTTP_PostWav(g_http_api,
                                              s_wav_buf, (size_t)wav_len,
                                              s_resp_buf, MUSIC_RESP_BUF_SIZE,
                                              &resp);

        if (ret == BSP_HTTP_ERR_ABORTED)
        {
            /* ★ 被新的上传请求取消了 —— 这不是失败 ★
             * 用户按了 KEY1 开始录第二段，主动顶掉了这一次。
             * 所以：不报错、不点故障灯，返回专门的"已被取代"结果。 */
            ESP_LOGW(TAG, "本次上传已被新的录音取代（用户重新录了）");
            result = APP_MUSIC_ERR_SUPERSEDED;
            break;
        }

        if (ret != BSP_HTTP_OK)
        {
            ESP_LOGE(TAG, "upload failed: %s", BSP_HTTP_RetStr(ret));
            break;
        }

        ESP_LOGI(TAG, "upload OK: %d bytes in %u ms",
                 resp.body_len, (unsigned)resp.elapsed_ms);

        /* ---------- 3. 解析 ---------- */
        int pr = App_Music_Parse(s_resp_buf, s_music);

        /* 顺手把后端的 text 字段抠出来（只是字符串，用 cJSON 找一次很便宜）。
         * ★ 单独抠而不是让 App_Music_Parse 一起返回：解析器刻意只认 music 字段，
         *   保持「设备只读 music」的约定；text 属于给人看的信息，放业务层更合适。 */
        {
            cJSON *root = cJSON_Parse(s_resp_buf);
            if (root != NULL)
            {
                const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
                if (cJSON_IsString(t) && t->valuestring != NULL)
                {
                    strncpy(s_last_text, t->valuestring, sizeof(s_last_text) - 1);
                    s_last_text[sizeof(s_last_text) - 1] = '\0';
                }

                const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "intent_source");
                if (cJSON_IsString(src) && src->valuestring != NULL)
                {
                    ESP_LOGI(TAG, "识别的意图原文: \"%s\"", s_last_text);
                    ESP_LOGI(TAG, "intent_source = %s %s", src->valuestring,
                             (strcmp(src->valuestring, "fallback") == 0)
                                 ? "(⚠️ 走了后端兜底，音乐不会随说话内容变化)"
                                 : "");
                }
                cJSON_Delete(root);
            }
        }

        if (pr != 0 && pr != -5)       /* -5 是"截断但仍可用"，允许继续 */
        {
            ESP_LOGE(TAG, "parse failed (%d)", pr);
            result = APP_MUSIC_ERR_PARSE;
            break;
        }

#if MUSIC_LOG_RESPONSE
        /* 后端的 text 字段（识别出的歌名/描述），以及诊断字段。
         * ★ intent_source 是排查「音乐永远一样」的关键：
         *     'llm'      = 真的用了模型
         *     'fallback' = 模型失败，用的是后端写死的默认编排（所以每次一样） */
        App_Music_Dump(s_music);
#endif

        /* ---------- 4. 初始化 KS 合成器 ----------
         * ★★ 必须走 App_Audio_SynthReload()，不能直接调 KS_Synth_Init() ★★
         *
         * 原因（审计发现的严重缺陷 S1）：KS_Synth_Init() 第一件事是
         * memset 整个 66KB 的合成器对象，而音频任务可能正在渲染同一个对象
         * → 空指针解引用 / 除零崩溃。所以初始化改成一条命令，由音频任务执行，
         * 这里等回执。
         *
         * ★ 本函数因此【不能】在音频任务上下文里被调用 ★
         *   它会等音频任务发回执，自己等自己会死锁。只该由按键任务调用。 */
        if (App_Audio_SynthReload(s_synth, s_music) != 0)
        {
            ESP_LOGE(TAG, "KS_Synth 初始化失败（音频任务侧）");
            result = APP_MUSIC_ERR_PARSE;
            break;
        }

        s_has_audio = 1;
        ESP_LOGI(TAG, "★ music ready: %.1f s, press KEY2 to play (loop)",
                 (double)Music_DurationMs(s_music) / 1000.0);

        result = APP_MUSIC_OK;

    } while (0);

    /* ★ 唯一出口：无论成功、失败、还是中途 break，都在这里解锁 ★ */
    backend_unlock();

    return result;
}

/*==============================================================================
 * KS 合成器 → 播放任务 的回调音源
 *============================================================================*/
/**
 * @brief 合成音源的 fill 回调
 * @note  直接把 KS 渲染结果填进播放任务给的缓冲。
 *        每次调用渲染 AUDIO_CHUNK_FRAMES 个样点（256ms），
 *        这个量在 240MHz 下约 3~8ms 就能算完，远小于 256ms 的预算，
 *        所以不会成为瓶颈。
 */
static int music_synth_fill(App_AudioSource_t *self, int16_t *out, size_t frames)
{
    (void)self;

    if (s_synth == NULL || s_music == NULL)
        return -1;

    int n = KS_Synth_Render(s_synth, out, frames);
    if (n <= 0)
        return -1;

    /* ★★ 一轮播完要告诉播放任务"本轮结束"（审计修复 S4）★★
     *
     * 原来这里无条件 return n（恒 > 0），于是播放任务里处理"音源结束"的
     * 分支永远不可达 —— 结果是 loop=1 实际【不循环】：
     * 第 128 步之后不再触发新音，已有声部约 2 秒衰减完，之后是纯数字静音，
     * 而 s_playing 仍是 1。用户听到的是"放完一遍就没声了"。
     *
     * 修法：两个条件都满足才算一轮结束：
     *   · KS_Synth_IsFinished()      —— 不再触发新音符
     *   · KS_Synth_ActiveVoices()==0 —— 尾音也衰减干净了
     * 第二个条件不能省：decay_seconds 最长 3.5 秒，若还有声部在响就重绕，
     * KS_Synth_Restart 会把所有声部置 FREE（等于硬切）→ 接缝处"咔"一声。
     *
     * ★ 这条修复依赖声部回收（S1）★ 否则 ActiveVoices 恒为 32，条件永不成立。 */
    if (KS_Synth_IsFinished(s_synth) && KS_Synth_ActiveVoices(s_synth) == 0)
    {
        ESP_LOGI(TAG, "音乐一轮播放结束（尾音已衰减完）");
        return -1;
    }

    /* 一轮播完之后返回 -1 让播放任务决定是循环还是收尾。
     * 简单起见：继续渲染（KS_Synth_Render 内部会在 finished 后静止），
     * 由播放任务的 loop 参数控制是否 rewind。
     * 所以这里只有在"确实算不出数据"时才返回负值。 */
    return n;
}

/**
 * @brief 合成音源的 rewind 回调
 * @note  ★ 「再按停止，下次播放从头开始」靠这个 ★
 *        除了重置合成器进度，还要重置所有声部（否则上一轮还在衰减的音
 *        会叠到新一轮开头，听感上像"上一首的尾巴没消"）。
 */
static void music_synth_rewind(App_AudioSource_t *self)
{
    (void)self;

    if (s_synth != NULL)
        KS_Synth_Restart(s_synth);
}

/** @brief 合成音源（单例，因为合成器只有一个） */
static App_AudioSource_t s_synth_src = {
    .fill   = music_synth_fill,
    .rewind = music_synth_rewind,
    .user   = NULL,
};

/*==============================================================================
 * 播放控制
 *============================================================================*/
int App_MusicApp_Play(void)
{
    if (!s_has_audio || s_synth == NULL)
    {
        ESP_LOGW(TAG, "no music to play (has_audio=%u)", s_has_audio);
        return -1;
    }

    /* ★ 循环播放 ★ 需求：KEY2 播放已有音频（循环播放）
     * 再按停止后，下次播放从头开始 —— 这一点由 music_synth_rewind 保证。 */
    return App_Audio_Play(&s_synth_src, PLAYBACK_LOOP_ENABLE, 1);
}

uint8_t App_MusicApp_HasAudio(void)
{
    return s_has_audio;
}

const char *App_MusicApp_GetLastText(void)
{
    return (s_last_text[0] != '\0') ? s_last_text : NULL;
}

void App_MusicApp_Dump(void)
{
    if (s_music == NULL)
    {
        ESP_LOGI(TAG, "no music loaded");
        return;
    }
    App_Music_Dump(s_music);
}
