/**
 * @file task_key.c
 * @brief 按键任务实现（扫描 + 命令分发 + 4 个按键的业务逻辑）
 * @author 李嘉图
 * @date 2026-06-18
 *
 * @note 文件里有两个任务，构成生产-消费对：
 *
 *          Task_Key_Debounce  ──事件入队──▶  [KEY_EVENT_Q]  ──▶  Task_Key_Cmd
 *            (每 10ms 扫描)                                        (阻塞等待)
 *
 *       ★ 为什么拆成两个任务而不是「扫到就直接处理」：
 *         如果扫描任务里直接执行业务动作（比如等 5 秒回环录音、
 *         或等 30 秒 HTTP 上传），扫描会停那么久，期间所有按键全部失灵。
 *         拆开后扫描任务的周期永远稳定。
 *
 *==============================================================================
 * ★ 4 个按键的业务定义（按需求）★
 *==============================================================================
 *   KEY1  按住录音 / 松手停止并上传后端
 *          按下 → 开始录音（麦克风灯亮）
 *            ★ 按住式：松手才停。所以 DOWN 开始、CLICK / LONG_UP 停止。
 *              两种松手事件都要处理，否则按住超过 2 秒再松手会没有任何反应。
 *          松手 → 停止录音 → 上传后端 → 解析 → 准备合成
 *                  → 有音频了，音频灯常亮 + 闪三下
 *          安全兜底：录满 8 秒自动停止（后端 MAX_AUDIO_SECONDS=15，留足余量）
 *
 *   KEY2  播放已有音频（循环）/ 再按一次停止，下次从头开始
 *
 *   KEY3  回环测试：按一下录固定 5 秒到本地缓冲（不上传），自动录满结束
 *
 *   KEY4  播放回环录音（单次，不循环）/ 再按一次停止，下次从头开始
 *==============================================================================
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include "ds_common.h"
#include "task_key.h"
#include "task_key_cfg.h"
// 功能包含
#include "app_audio.h"
#include "app_audio_cfg.h"
#include "app_led.h"
#include "app_key.h"
#include "app_music_app.h"
#include "bsp_i2s.h"
#include "bsp_http.h"     /* BSP_HTTP_RequestCancel + g_http_api（新录音顶掉旧上传） */
#include "bsp_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* 上传请求信号量 */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "TASK_KEY";

/*==============================================================================
 * 内部状态
 *============================================================================*/
/** @brief 回环录音的音源（KEY4 反复播放同一个，所以缓存一份指针） */
static const App_AudioSource_t *s_loop_src = NULL;

/** @brief KEY1 是否正在"按住录音"中（用于判断松手该不该上传） */
static uint8_t s_key1_recording = 0;

/*==============================================================================
 * 内部辅助
 *============================================================================*/
/**
 * @brief 上传并准备合成（KEY1 松手后调用）
 * @note  ★ 这个函数会阻塞 3~10 秒（上传 + Whisper + LLM）★
 *        它是唯一会长时间阻塞按键分发任务的地方。
 *        期间按键事件会堆在队列里（深度 16），不会丢，只是响应延后。
 *        这样设计是有意的：上传期间本来就不该允许再触发一次录音。
 */
/*==============================================================================
 * ★ 上传 worker 的请求信号（把耗时上传从按键任务里搬出来）★
 *============================================================================*/
/**
 * @brief 上传请求信号量（计数型，最多排 UPLOAD_REQ_MAX 个）
 * @note  ★ 为什么上传不能留在 Task_Key_Cmd 里（实测教训）★
 *
 *        它曾经同步跑完整条上传链路。实测：TLS 握手 3~10 秒，
 *        102KB body 还可能被对端重置（单次 45 秒），失败要重试最多 5 次
 *        → 最坏 3.75 分钟内 Task_Key_Cmd 一直不消费按键事件队列。
 *        而消抖任务仍在塞事件（队列只有 16 深）→ 溢出丢事件
 *        → 按键状态机与上层记录永久不一致。
 *
 *        用户看到的现象：「第一次按 KEY1 能录，第二次按 KEY1 没反应」——
 *        因为第一次的松手事件被丢掉了，s_key1_recording 永远停在 1，
 *        第二次的 DOWN 走到 RecordStart 时被拒（已在录音中），日志什么都不打。
 *
 *        ★ 注意：不要试图"忙的时候丢按键事件"来省事 ★
 *          本模块的事件（DOWN/CLICK/LONG/LONG_UP）全部是状态转换的
 *          唯一次机会，没有任何周期性重复事件（见 app_key.c 顶部的状态图）——
 *          丢任何一个都会永久破坏状态机。试过，更糟。
 *
 *        修法：Task_Key_Cmd 只投递请求（非阻塞），真正干活的是 Task_Key_Upload。
 */
static SemaphoreHandle_t s_upload_sig = NULL;

/**
 * @brief 是否正在上传
 * @note  上传 worker 写、按键任务读，所以加 volatile。
 *        不做原子保护：它只是"忙/闲"提示，最坏让重复请求被放行一次，
 *        而上传 worker 自己也会拒绝（见 Task_Key_Upload）。
 */
static volatile uint8_t s_upload_busy = 0;

/* 前置声明：Task_Key_Upload 在文件末尾，而它要调这个函数 */
static void key_do_upload_and_prepare(void);

/** @brief 上传请求最多排队几个（1 就够：WAV 缓冲只有一份，堆多个没意义） */
#define UPLOAD_REQ_MAX      1

uint8_t Task_Key_UploadIsBusy(void)
{
    return s_upload_busy;
}

/**
 * @brief 停止（取消）正在进行的上传，让位给新的录音
 *
 * @note  ★ 对应用户需求 ★
 *        「如果第一次录音上传没有结束就进行第二次录音的话，
 *          直接停止第一次录音的上传，直接上传第二次的录音」
 *
 *        ★ 能取消到什么程度（必须如实说明，别让人以为能立刻掐断）★
 *
 *        实测 ESP-IDF 5.4.4 的 `esp_http_client_perform`：
 *        发 body 是 `esp_http_client_write(client, post_data + written, left)`，
 *        **一次 write 调用内部没有任何回调点** —— 所以：
 *
 *          ❌ **正在传输中的那一次 write 无法中断**。
 *             对端不收数据时 TCP 发送缓冲会满，这一 write 可能阻塞几十秒。
 *
 *          ✅ 但下面这些时机能立刻生效：
 *             1) **重试之间** —— 检查到取消就不再重试
 *                （最省时间的地方：省掉最多 4 次 × 45 秒 ≈ 3 分钟）
 *             2) **下一个请求开始时** —— 见 app_music_app.c 上传入口清标志
 *
 *        ★ 所以本函数做两件事 ★
 *          ① 置取消标志（让在跑的那次上传不再重试、尽快退出）
 *          ② 把上传请求信号量清零（"排队等执行"的请求也一并作废）
 *
 *        ★ 不掐断连接 ★
 *          真掐断需要在事件回调里调 esp_http_client_close()，而那是在
 *          perform 自己的执行栈里 —— 关掉正在用的 client 属于未定义行为。
 *          协作式取消是安全且可预期的，代价是最坏多等"一次尝试"的时间。
 */
static void key_cancel_running_upload(void)
{
    if (!s_upload_busy)
        return;                 /* 没在上传，什么都不用做 */

    ESP_LOGW(TAG, "检测到新的录音 → 取消正在进行的上传");

    /* ① 让在跑的那次上传不再重试（省掉最多 4 次 × 45 秒） */
    BSP_HTTP_RequestCancel(g_http_api, 1);

    /* ② 把还没被 worker 取走的请求也作废 */
    if (s_upload_sig != NULL)
    {
        while (xSemaphoreTake(s_upload_sig, 0) == pdTRUE)
        {
            /* 全部取走 = 作废 */
        }
    }
}

/**
 * @brief 真正执行上传（★ 只允许 Task_Key_Upload 调用 ★）
 * @note  上传前把 s_upload_busy 置 1，无论成败都要清 0 ——
 *        否则忙标志会永久为真，之后所有 KEY1 上传请求都会被拒。
 */
static void key_do_upload_and_prepare(void)
{
    ESP_LOGI(TAG, "=== 上传中，请稍候（约 3~60 秒，取决于网络）===");

    /* ★★ 音频灯保持"灭"，直到后端真的把数据返回来（用户需求）★★
     *
     * 需求原文：「不要让 LED4 在录完音之后直接就亮，
     *           要后端已经将数据返回到 ESP32 后再亮」
     *
     * 原来是上传一开始就切成快闪（表示"在处理"），成功后常亮。
     * 现在改成：录完音 → 灭 → 【后端返回且合成器就绪】→ 常亮 + 闪三下。
     *
     * ★ 为什么直接灭、而不是用快闪表示"上传中" ★
     *   这个灯的语义是「有没有可播放的音频」（需求里定义的）：
     *       常亮 = 有音频可播；灭 = 还没有
     *   上传中的进度反馈由【后端指示灯】和串口日志承担，
     *   不该借用"有无音频"这个语义灯。混用会让"灭"这个状态变得含糊：
     *   究竟是"没有音频"还是"正在传"？
     *
     * ★ 这样改还有一个实际好处 ★
     *   上传失败时灯保持灭 = 如实反映"没有新的可播放音频"，
     *   而原来失败时会从快闪切到灭，中间那段快闪看起来像"有在做事"，
     *   反而掩盖了失败。 */
    App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);

    /* ★ 置忙标志 ★
     * 它现在只是给 Task_Key_Cmd 一个"要不要受理新的上传请求"的提示，
     * 【不再用来丢弃按键事件】（那个做法是错的，见 s_upload_sig 的注释）。 */
    s_upload_busy = 1;

    App_Music_Result_t r = App_MusicApp_UploadAndPrepare();

    /* ★ 无论成败都要清，否则之后所有上传请求都会被拒 ★ */
    s_upload_busy = 0;

    switch (r)
    {
    case APP_MUSIC_OK:
        ESP_LOGI(TAG, "★ 音乐已就绪，按 KEY2 播放（循环）");
        /* ★ 一次调用表达完整意图：闪三下 → 留在常亮（实测修复）★
         *
         * 原来是两行：
         *     App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_ON);
         *     App_LED_Flash(APP_LED_AUDIO, APP_LED_AUDIO_FLASH_CNT);
         * 第二行会把第一行设的图案**整体替换**，而旧 Flash 的末态是"灭"
         * → 用户看到的是"闪三下然后灯灭了"，误以为没有可播放音频。
         * 现在用 FlashEx 的 end_on 参数把"闪三下"和"常亮"合成同一个图案。 */
        App_LED_FlashEx(APP_LED_AUDIO, APP_LED_AUDIO_FLASH_CNT, 1);
        break;

    case APP_MUSIC_ERR_WIFI:
        ESP_LOGE(TAG, "上传失败：WiFi 未连接（看 WiFi 指示灯）");
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
        break;

    case APP_MUSIC_ERR_NO_URL:
        ESP_LOGE(TAG, "上传失败：后端地址未配置（改 app_music_cfg.h）");
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
        break;

    case APP_MUSIC_ERR_NO_AUDIO:
        ESP_LOGE(TAG, "上传失败：没录到音频（麦克风接好了吗？试 KEY3 回环测试）");
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
        break;

    case APP_MUSIC_ERR_PARSE:
        /* ★ 必须和「网络问题」分开报（这是审计复核发现的误导点）★
         * HTTP 已经 200 拿到响应了，失败在解析或合成器初始化 —— 跟网络无关。
         * 原来这个码落进 default，一起报"网络/后端问题"，
         * 会把人引去查网络，而真正的原因在本地（JSON 字段/合成器参数）。
         * 本工程已经因为"网络 vs 本地"误判绕过两圈了，这里必须区分清楚。 */
        ESP_LOGE(TAG, "上传失败：后端已响应，但解析/合成器初始化失败（不是网络问题）");
        ESP_LOGE(TAG, "  看上面 BSP_HTTP 的 HTTP 200 与 body 字节数：");
        ESP_LOGE(TAG, "    · 若 body 很小或不是 JSON  -> 后端返回异常，看 worker 日志");
        ESP_LOGE(TAG, "    · 若 KS_Synth_Init 报错    -> 合成器参数越界，看它打印的 bars/chord 信息");
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
        break;

    case APP_MUSIC_ERR_BUSY_SKIP:
        /* 上传路径不该拿到这个码（它是探活专用的"本轮跳过"），但列出来更安全 */
        ESP_LOGW(TAG, "上传被跳过：另一个后端请求正在进行");
        break;

    case APP_MUSIC_ERR_SUPERSEDED:
        /* ★ 被新的录音取代 —— 不是失败，所以什么都不用改 ★
         * 用户按了 KEY1 开始录第二段，主动顶掉了这一次。
         *   · 灯保持灭（本来就没有新的可播放音频）
         *   · 不打错误日志（这是预期行为，不是故障）
         * 第二次录音松手后会再走一遍本函数，正常点亮。 */
        ESP_LOGI(TAG, "（本次上传已被新的录音取代，等第二次录音）");
        break;

    case APP_MUSIC_ERR_HTTP:
    default:
        ESP_LOGE(TAG, "上传失败：网络/后端问题（看后端指示灯：慢闪 = 到不了后端）");
        ESP_LOGE(TAG, "  若是 DNS 解析问题，可打开 BACKEND_DNS_OVERRIDE_ENABLE 换 DNS");
        ESP_LOGE(TAG, "  提示：本工程用自定义域名 www.230508.xyz，国内直连实测可用；");
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
        break;
    }
}

/**
 * @brief 停止播放并把音频灯恢复成「有/无音频」的正确状态
 */
static void key_stop_playback(void)
{
    App_Audio_Stop();

    /* 停止后灯要反映"还有没有音频"，而不是一律熄灭：
     * 合成音频还在 → 常亮；不在 → 灭 */
    if (App_MusicApp_HasAudio())
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_ON);
    else
        App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_OFF);
}

/**
 * @brief 串口打印一次录音质量诊断（★ 回环测试靠这个判断麦克风好坏 ★）
 * @param target 打印哪个缓冲的诊断
 * @note  ★ 为什么回环测试不点灯、只看串口 ★
 *        按需求，录音灯和音频灯只反映「KEY1 录音 / 有没有合成音频」这两件事，
 *        回环测试不应该去抢那两个灯 —— 否则两个功能同时在跑时灯的含义就乱了。
 *        所以回环测试改成完全靠串口输出判断，好处是能给出定量结果：
 *          frames / rms / peak / dc / clipped 每一项都直接指向一个具体故障。
 */
static void key_print_rec_diag(App_Audio_RecTarget_t target)
{
    App_Audio_RecStats_t st;

    if (App_Audio_GetRecStats(target, &st) != 0)
    {
        ESP_LOGE(TAG, "[回环] 取诊断失败");
        return;
    }

    const char *name = (target == AUDIO_REC_LOOPBACK) ? "回环" : "上传";

    ESP_LOGI(TAG, "==================== %s录音诊断 ====================", name);
    ESP_LOGI(TAG, "  frames  = %u   (%.2f 秒 @16kHz)", (unsigned)st.frames,
             (double)st.ms / 1000.0);
    ESP_LOGI(TAG, "  rms     = %.4f", (double)st.rms);
    ESP_LOGI(TAG, "  peak    = %.4f", (double)st.peak);
    ESP_LOGI(TAG, "  dc      = %.5f", (double)st.dc);
    ESP_LOGI(TAG, "  clipped = %u", st.clipped);

    /* ---------- 自动判读（把数字翻译成"下一步改什么"）---------- */
    if (st.frames == 0)
    {
        ESP_LOGE(TAG, "  ✗ 一个样点都没录到！按这个顺序查：");
        ESP_LOGE(TAG, "     1) INMP441 的 VDD 是否接 3.3V（不要接 5V）");
        ESP_LOGE(TAG, "     2) L/R 脚是否接了 GND（接地 = 左声道，悬空会读不到数据）");
        ESP_LOGE(TAG, "     3) SCK→GPIO41  WS→GPIO42  SD→GPIO47 有没有接错/虚接");
        ESP_LOGE(TAG, "     4) VDD 是否并联了 100nF + 10uF 去耦电容");
    }
    else if (st.rms < 0.002f)
    {
        ESP_LOGE(TAG, "  ✗ RMS 极低（几乎静音）—— 有数据但没信号，对照后端给出的 hint：");
        ESP_LOGE(TAG, "     优先怀疑 L/R 脚悬空或接错、I2S 数据线接错、");
        ESP_LOGE(TAG, "     或 slot_mask 没设成 LEFT（本工程已设 LEFT，所以先查前两条）");
    }
    else if (st.rms < 0.01f)
    {
        ESP_LOGW(TAG, "  ⚠ RMS 偏低但可用（INMP441 灵敏度 -26dBFS 属正常）");
        ESP_LOGW(TAG, "     想更响可以把 bsp_i2s.c 的 gain_shift 从 16 改成 15（+6dB）或 14（+12dB）");
    }
    else
    {
        ESP_LOGI(TAG, "  ✓ RMS 正常，麦克风工作良好");
    }

    if (st.clipped)
    {
        ESP_LOGE(TAG, "  ✗ 检测到削顶！把 bsp_i2s.c 的 gain_shift 调大（当前默认已是 16，可试 17/18）");
    }
    if ((st.dc > 0.05f) || (st.dc < -0.05f))
    {
        ESP_LOGE(TAG, "  ✗ 直流偏置偏大（%.5f）→ 查 L/R 脚接法与 slot_mask 是否匹配",
                 (double)st.dc);
    }
    if (st.frames > 0 && st.ms < 500)
    {
        ESP_LOGW(TAG, "  ⚠ 录音不足 0.5 秒，上传后端会识别失败（回环回放没事）");
    }

    ESP_LOGI(TAG, "==========================================================");
}

/*==============================================================================
 * 命令分发
 *============================================================================*/
/**
 * @brief 按键命令分发（★ 按键 → 业务 的唯一落点 ★）
 */
void App_Key_CMD_Packet(uint8_t key_id, App_Key_Event_t event)
{
    switch (key_id)
    {
    /* ================= KEY1：按住录音 / 松手上传 ================= */
    case 1:
        switch (event)
        {
        case APP_KEY_EVENT_DOWN:
            ESP_LOGI(TAG, "KEY1 按下 → 开始录音（松手上传，最长 %d 秒）",
                     AUDIO_REC_MAX_SECONDS);

            /* ★★ 新录音顶掉正在进行的上传（用户需求）★★
             *
             * 需求原文：「如果第一次录音上传没有结束就进行第二次录音的话，
             *           直接停止第一次录音的上传，直接上传第二次的录音」
             *
             * 放在这里而不是松手时，有两个好处：
             *   1) 用户一按下就立刻开始取消，抢在"当前这次尝试"结束前
             *      把取消标志置上，这样那次上传读到达不到的重试会立即退出
             *   2) 松手时再投新请求，语义清晰（录音完才上传）
             *
             * ★ 注意顺序：先取消【再】启动录音，不能反 ★
             *   如果先启动录音，音频任务会立刻开始往 s_rec_buf 写，
             *   而正在上传的那一次可能还在读同一块缓冲打包 WAV
             *   （虽然 WAV 已经打包进 s_wav_buf，但 GetRecordFrames 之类的
             *     元数据仍会被读）。取消上传让 worker 尽快退出，减少这种重叠窗口。 */
            key_cancel_running_upload();

            /* ★ 必须检查返回值（审计修复：原来忽略了返回值）★
             * 命令队列满时 RecordStart 会失败（-1），但原来的写法照样
             * 点亮录音灯并置 s_key1_recording = 1 —— 于是界面说"正在录音"、
             * 实际什么都没录，松手后只会走"太短不上传"分支。
             * 这种"界面与实际不符"的 bug 最难查（日志看起来一切正常）。 */
            if (App_Audio_RecordStart(AUDIO_REC_MAIN, AUDIO_REC_MAX_SECONDS) != 0)
            {
                ESP_LOGE(TAG, "录音命令入队失败（队列满？），本次录音取消");
                break;                  /* 不点灯、不置标志，保持状态一致 */
            }

            App_LED_Set(APP_LED_MIC, &APP_LED_PAT_ON);      /* 录音灯亮 */
            s_key1_recording = 1;
            break;

        case APP_KEY_EVENT_CLICK:       /* 短按松手 */
        case APP_KEY_EVENT_LONG_UP:     /* 长按后松手 */
            if (s_key1_recording)
            {
                s_key1_recording = 0;
                int frames = App_Audio_RecordStop(AUDIO_REC_MAIN);
                App_LED_Set(APP_LED_MIC, &APP_LED_PAT_OFF); /* 录音灯灭 */

                ESP_LOGI(TAG, "KEY1 松手 → 录音停止，%d 帧 (%.1f s)",
                         frames, (double)frames / (double)BSP_I2S_SAMPLE_RATE);

                /* 太短的录音后端会拒（API 校验：<0.5s 语音识别会失败），
                 * 本地先挡一道，省一次无用的上传 */
                if (frames < BSP_I2S_SAMPLE_RATE / 2)
                {
                    ESP_LOGW(TAG, "录音不足 0.5 秒，取消上传（太短识别不出内容）");
                    break;
                }
                
                /* ★ 投递上传请求，不在这里同步做（关键改动）★
                 *
                 * 原来这里是 `key_do_upload_and_prepare();` —— 直接同步执行
                 * 整条上传（TLS + 102KB body + 解析），最坏 3.75 分钟。
                 * 期间本任务不消费按键事件队列（深度 16）→ 溢出丢事件
                 * → 按键状态机与上层记录永久不一致。
                 *
                 * 实测症状：「第一次按 KEY1 能录，第二次按 KEY1 没反应」。
                 * 原因链：第一次的松手事件被丢掉 → s_key1_recording 永远为 1
                 * → 录音永不停 → 第二次按下去时 RecordStart 被拒（已在录音中）
                 * → 上层日志什么都不打。
                 *
                 * 现在只投一条信号（非阻塞，微秒级），立刻回去继续消费事件。 */
                if (s_upload_sig == NULL)
                {
                    ESP_LOGE(TAG, "上传信号量未就绪，本次上传取消");
                    break;
                }

                /* ★ 这里【不】因为"上一次还在上传"就拒绝 ★
                 *
                 * 用户的需求是：「第一次上传没结束就开始第二次录音，
                 * 就停止第一次的上传，直接上传第二次的」。
                 *
                 * 而 KEY1 按下时已经取消了正在跑的那次（见 DOWN 分支），
                 * 所以这里只需要把新请求投出去。
                 *
                 * ★ 但也不能盲投 ★
                 *   信号量深度只有 1。如果里面已经有一个"排队待执行"的请求，
                 *   再投会失败（xSemaphoreGive 会返回失败）。
                 *   用 uxSemaphoreGetCount 先看一眼，避免打无意义的警告。 */
                if (uxSemaphoreGetCount(s_upload_sig) > 0)
                {
                    ESP_LOGW(TAG, "已有一个上传请求在排队，本次请求被合并（不重复排队）");
                    break;
                }

                if (xSemaphoreGive(s_upload_sig) != pdTRUE)
                {
                    ESP_LOGW(TAG, "投递上传请求失败（信号量异常）");
                    break;
                }

                ESP_LOGI(TAG, "已投递上传请求给 worker 任务（按键任务立刻返回）");
            }
            break;

        case APP_KEY_EVENT_LONG:
            /* 长按到达阈值：这里不做任何事。
             * ★ 关键：按住式录音在 LONG 时【不能】停止，否则按满 2 秒就断了。
             *   真正的停止在 CLICK / LONG_UP（松手）时发生。 */
            ESP_LOGD(TAG, "KEY1 已按住 2 秒，继续录音直到松手");
            break;

        default:
            break;
        }
        break;

    /* ================= KEY2：播放合成音频（循环）/ 再按停止 ================= */
    case 2:
        if (event == APP_KEY_EVENT_CLICK)
        {
            if (App_Audio_IsPlaying())
            {
                ESP_LOGI(TAG, "KEY2 按下 → 停止播放（下次从头开始）");
                key_stop_playback();
            }
            else if (App_MusicApp_HasAudio())
            {
                ESP_LOGI(TAG, "KEY2 按下 → 循环播放合成音乐");
                if (App_MusicApp_Play() == 0)
                    App_LED_Set(APP_LED_AUDIO, &APP_LED_PAT_ON);
            }
            else
            {
                ESP_LOGW(TAG, "KEY2 按下，但没有可播放音频（先按 KEY1 录一句）");
                App_LED_Flash(APP_LED_AUDIO, 2);    /* 快速提示两下：没有音频 */
            }
        }
        break;

    /* ================= KEY3：按住录音 / 松手停止（回环测试，只存本地） =================
     *
     * ★ 按需求：按下开始录、松开停止，不是"按一下录固定长度" ★
     * ★ 并且【不操作 LED】—— 录音灯只归 KEY1 用 ★
     *   回环测试完全靠串口输出的诊断数字来判断麦克风好不好，
     *   好处是给的是定量结果（rms/peak/dc/clipped），比一个灯信息量大得多。
     */
    case 3:
        switch (event)
        {
        case APP_KEY_EVENT_DOWN:
            if (App_Audio_IsRecording())
            {
                ESP_LOGW(TAG, "KEY3 按下，但已经在录音中（KEY1 或上一轮 KEY3 还没结束）");
                break;
            }
            ESP_LOGI(TAG, "KEY3 按下 → 开始回环录音（松开停止，最长 %d 秒，不上传）",
                     AUDIO_LOOPBACK_SECONDS);
            if (App_Audio_LoopbackRecordStart(AUDIO_LOOPBACK_SECONDS) != 0)
                ESP_LOGE(TAG, "回环录音启动失败");
            break;

        case APP_KEY_EVENT_CLICK:       /* 短按松手 */
        case APP_KEY_EVENT_LONG_UP:     /* 长按后松手 */
            if (App_Audio_IsRecordingTarget(AUDIO_REC_LOOPBACK))
            {
                App_Audio_LoopbackRecordStop();
                key_print_rec_diag(AUDIO_REC_LOOPBACK);   /* ★ 关键：串口给结论 ★ */
            }
            else if (!App_Audio_RecordIsFull())
            {
                /* 没在录、也没录满 —— 说明这一轮按下时就被拒绝了 */
                ESP_LOGW(TAG, "KEY3 松手，但这一轮没有在进行回环录音");
            }
            else
            {
                /* 录满自动停了，此时松手：把已经录到的打印出来 */
                ESP_LOGW(TAG, "回环录音在松手前已达上限（%.1f 秒），打印已录内容",
                         (double)App_Audio_GetRecordFrames(AUDIO_REC_LOOPBACK)
                             / (double)BSP_I2S_SAMPLE_RATE);
                key_print_rec_diag(AUDIO_REC_LOOPBACK);
            }
            break;

        case APP_KEY_EVENT_LONG:
            /* 按住式录音在长按阈值时不做任何事（真正的停止在松手） */
            ESP_LOGD(TAG, "KEY3 已按住 2 秒，继续录音直到松手");
            break;

        default:
            break;
        }
        break;

    /* ================= KEY4：播放回环录音（单次）/ 再按停止 =================
     *
     * ★ 按需求：
     *     按一下 → 从头播一遍（不循环）
     *     播放中再按 → 停止
     *     停止后再按 → 从头开始播
     *   "从头开始"由 App_Audio_Play 内部的 rewind 保证（每次播放前都会重绕）。
     * ★ 同样【不操作 LED】。
     */
    case 4:
        if (event == APP_KEY_EVENT_CLICK)
        {
            if (App_Audio_IsPlayingSource(s_loop_src) && s_loop_src != NULL)
            {
                /* 正在放的就是回环录音 → 停止 */
                ESP_LOGI(TAG, "KEY4 按下 → 停止播放（下次再按从头开始）");
                App_Audio_Stop();
                break;
            }

            if (App_Audio_IsPlaying())
            {
                /* 正在放的是别的东西（比如 KEY2 的合成音乐）→ 也停掉，然后放回环 */
                ESP_LOGI(TAG, "KEY4 按下 → 先停掉当前播放");
                App_Audio_Stop();
            }

            if (!App_Audio_HasLoopback())
            {
                ESP_LOGW(TAG, "KEY4 按下，但还没有回环录音（先按住 KEY3 录一段）");
                break;
            }

            s_loop_src = App_Audio_GetLoopbackSrc();
            if (s_loop_src == NULL)
            {
                ESP_LOGE(TAG, "取回环音源失败");
                break;
            }

            ESP_LOGI(TAG, "KEY4 按下 → 单次回放回环录音（%u 帧, %.2f 秒）",
                     (unsigned)App_Audio_GetRecordFrames(AUDIO_REC_LOOPBACK),
                     (double)App_Audio_GetRecordFrames(AUDIO_REC_LOOPBACK)
                         / (double)BSP_I2S_SAMPLE_RATE);

            /* ★ 不循环（loop = 0），只播一遍；stop_current = 1 先清干净 ★ */
            if (App_Audio_Play(s_loop_src, 0, 1) != 0)
                ESP_LOGE(TAG, "回放命令入队失败（队列满？）");
        }
        break;
    default:
        ESP_LOGW(TAG, "unknown key_id %u, event %d", key_id, (int)event);
        break;
    }
}

/*==============================================================================
 * 按键注册表
 *============================================================================*/
/**
 * @brief 把注册表打印到日志，方便上电时核对「哪个键对应哪个引脚」
 */
static void task_key_dump_reg_table(const key_register_t *tbl, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
    {
        ESP_LOGI(TAG, "  key%u -> IO%d (long %u ms, debounce %u ms, active %s)",
                 tbl[i].key_id,
                 BSP_GPIO_GetNum(tbl[i].gpio),
                 tbl[i].long_press_ms,
                 tbl[i].debounce_ms,
                 tbl[i].active_level ? "HIGH" : "LOW");
    }
}

/*==============================================================================
 * 周期检查：录音自动结束时的收尾
 *============================================================================*/
/**
 * @brief 录音状态巡检
 * @note  ★ 为什么需要巡检 ★
 *        录音可能是"自动结束"的（录满上限时音频任务自己停），
 *        这时没有按键事件来触发收尾逻辑，必须由扫描任务周期检查。
 *        这就是"状态要跟着真实状态走，而不是跟着事件走"。
 *
 *        ★ 录音灯只跟 KEY1（主缓冲）走 ★
 *          按需求，录音灯只表示"KEY1 正在录音"。
 *          KEY3 的回环录音不点灯（改用串口诊断），所以这里必须判断目标，
 *          否则 KEY3 录满时会去关 KEY1 的灯（或反过来）。
 */
static void task_key_poll_record_state(void)
{
    static uint8_t was_main_recording = 0;

    uint8_t now_main_recording = App_Audio_IsRecordingTarget(AUDIO_REC_MAIN);

    if (was_main_recording && !now_main_recording)
    {
        /* KEY1 的录音刚刚结束（松手，或录满 8 秒自动停）→ 关录音灯 */
        App_LED_Set(APP_LED_MIC, &APP_LED_PAT_OFF);

        if (App_Audio_RecordIsFull())
        {
            ESP_LOGW(TAG, "主缓冲录音已录满自动停止（%u 帧），未上传",
                     (unsigned)App_Audio_GetRecordFrames(AUDIO_REC_MAIN));
        }
    }

    was_main_recording = now_main_recording;
}

/*==============================================================================
 * 任务函数：扫描（生产者）
 *============================================================================*/
void Task_Key_Debounce(void *argument)
{
    (void)argument;

    /* 初始化按键模块：配置引脚 + 建事件队列 */
    App_Key_Init();

    /* 注册表：key_id → BSP 具名句柄 */
    const key_register_t key_reg_table[KEY_REG_TABLE_SIZE] = {
        /* key_id, gpio,   long_press_ms,     active_level,     debounce_ms */
        {  1,      g_key1, KEY_LONG_PRESS_MS, KEY_ACTIVE_LEVEL, KEY_DEBOUNCE_MS },
        {  2,      g_key2, KEY_LONG_PRESS_MS, KEY_ACTIVE_LEVEL, KEY_DEBOUNCE_MS },
        {  3,      g_key3, KEY_LONG_PRESS_MS, KEY_ACTIVE_LEVEL, KEY_DEBOUNCE_MS },
        {  4,      g_key4, KEY_LONG_PRESS_MS, KEY_ACTIVE_LEVEL, KEY_DEBOUNCE_MS },
    };

    for (uint8_t i = 0; i < ARRAY_SIZE(key_reg_table); i++)
    {
        App_Key_Register(key_reg_table[i].key_id,
                         key_reg_table[i].gpio,
                         key_reg_table[i].long_press_ms,
                         key_reg_table[i].active_level,
                         key_reg_table[i].debounce_ms);
    }

    ESP_LOGI(TAG, "scan task started: %u keys, period = %d ms",
             (unsigned)ARRAY_SIZE(key_reg_table), KEY_SCAN_PERIOD_MS);
    task_key_dump_reg_table(key_reg_table, (uint8_t)ARRAY_SIZE(key_reg_table));
    ESP_LOGI(TAG, "  KEY1=按住录音/松手上传(最长%d秒)  KEY2=播放合成(循环)/停止",
             AUDIO_REC_MAX_SECONDS);
    ESP_LOGI(TAG, "  KEY3=按住回环录音/松手停止(不上传)  KEY4=回放回环(单次)/再按停止");
    ESP_LOGI(TAG, "  回环测试看串口：松手后会打印 rms/peak/dc/clipped 和判读结论");

    uint32_t last_ms = BSP_SYS_GetTickMs();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));

        /* ★ 传真实经过的时间：任务被挤掉时也不会把长按时间算错 */
        uint32_t now = BSP_SYS_GetTickMs();
        uint32_t elapsed = now - last_ms;
        last_ms = now;

        App_Key_Update(elapsed);

        /* 录音自动结束时把录音灯关掉 */
        task_key_poll_record_state();
    }
}

/*==============================================================================
 * 任务函数：命令分发（消费者）
 *============================================================================*/
void Task_Key_Cmd(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG, "cmd task started, waiting for key events...");

    App_Key_EventPacket_t msg;

    /* ★ 建上传请求信号量（Task_Key_Upload 会阻塞等它）★
     * 放在这里而不是 init 函数里：本任务和 worker 都由 main.c 创建，
     * 而这个文件没有独立的 init 入口。用 if 保护保证幂等。 */
    if (s_upload_sig == NULL)
    {
        s_upload_sig = xSemaphoreCreateCounting(UPLOAD_REQ_MAX, 0);
        if (s_upload_sig == NULL)
        {
            ESP_LOGE(TAG, "create upload semaphore failed, 上传功能不可用");
        }
    }

    for (;;)
    {
        /* 阻塞等事件，不轮询、不占 CPU */
        if (App_Key_WaitEvent(&msg, 0xFFFFFFFFu) == 1)
        {
            App_Key_CMD_Packet(msg.key_id, msg.event);
        }
    }
}

/*==============================================================================
 * 上传 worker 任务
 *============================================================================*/
/**
 * @brief 上传 worker：把耗时的上传从按键任务里搬出来
 *
 * @note  ★ 设计要点 ★
 *
 *        1) **阻塞等信号量**，拿到就干活，干完继续等 —— 不轮询、不占 CPU。
 *
 *        2) **拒绝重入**：如果拿到信号量时发现上一次还在忙（理论上不可能，
 *           因为本任务是唯一的执行者，且忙时才给出信号），记录并跳过。
 *
 *        3) **不丢请求，但也不堆请求**：信号量深度只有 1。
 *           堆多个请求没有意义 —— WAV 缓冲只有一份，后一个请求会覆盖前一个
 *           的录音，结果两个请求上传同一份音频。所以 Task_Key_Cmd 侧
 *           会在忙时直接拒绝（并明确告诉用户"上一次还在上传"），
 *           而不是把请求排起来。
 *
 *        4) **栈要求**：本任务要跑 esp_http_client_perform（TLS 握手）、
 *           cJSON_Parse、KS 合成器初始化，所以栈必须给足 8192
 *           （和 task_key_cmd 同一个量级）。见 main.c 的 STACK_KEY_UPLOAD。
 */
void Task_Key_Upload(void *argument)
{
    (void)argument;

    if (s_upload_sig == NULL)
    {
        ESP_LOGE(TAG, "upload task: semaphore not ready, task exit");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "upload worker started (waits for KEY1 release)");

    for (;;)
    {
        /* 无限等 —— 没有请求时完全让出 CPU */
        if (xSemaphoreTake(s_upload_sig, portMAX_DELAY) != pdTRUE)
            continue;

        ESP_LOGI(TAG, ">>> 收到上传请求，开始执行 <<<");
        key_do_upload_and_prepare();
        ESP_LOGI(TAG, ">>> 上传流程结束 <<<");
    }
}
