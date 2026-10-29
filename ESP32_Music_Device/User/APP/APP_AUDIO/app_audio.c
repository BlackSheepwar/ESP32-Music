/**
 * @file app_audio.c
 * @brief 音频应用层实现（录音缓冲 + 统一播放任务）
 * @author 李嘉图
 * @date 2026-06-26
 *
 *==============================================================================
 * ★ 关键设计说明 ★
 *==============================================================================
 *
 *  1) 播放任务同时管麦克风和扬声器
 *     I2S0（麦克风）和 I2S1（扬声器）虽然物理上独立，但"同一时刻只做一件事"
 *     更简单也更省 CPU。所以录音和播放都在这一个任务里做，
 *     其它任务通过状态位/队列请求，不直接碰 I2S。
 *     好处是不会出现两个任务同时写 I2S 导致 DMA 数据交错。
 *
 *  2) 播放节流靠 i2s_channel_write 阻塞
 *     不需要自己算时间戳、也不需要用 vTaskDelay 凑时序：
 *     DMA 总共只有约 90ms 的缓冲，写满之后 i2s_channel_write 会阻塞，
 *     于是整个循环自动被限速到 16kHz 的实时速度。
 *     这也是为什么 AUDIO_CHUNK_FRAMES 不能太小（调用开销）也不能太大
 *     （按键响应变迟钝）。
 *
 *  3) 录音缓冲和回环缓冲都在 PSRAM
 *     256KB × 2 放内部 RAM 会把 WiFi/TLS 挤爆，详见 app_audio_cfg.h。
 *
 *  4) 播放命令走队列，不直接执行
 *     按键任务调 App_Audio_Play() 只是入队（不阻塞），
 *     真正的 I2S 写入在音频任务里。这样按键响应不会被 I2S 拖慢。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>       /* sqrt：录音诊断里算 RMS */
#include "ds_common.h"
#include "app_audio.h"
#include "app_audio_cfg.h"
// 功能包含
#include "bsp_i2s.h"
#include "bsp_sys.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"     /* portMUX_TYPE：保护录音共享状态 */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "APP_AUDIO";

/** @brief 播放命令队列深度 */
#define AUDIO_CMD_QUEUE_LEN     8

/** @brief 录音缓冲最大样点数 */
#define AUDIO_REC_MAX_FRAMES    (BSP_I2S_SAMPLE_RATE * AUDIO_REC_MAX_SECONDS)

/** @brief 回环缓冲最大样点数 */
#define AUDIO_LOOPBACK_MAX_FRAMES (BSP_I2S_SAMPLE_RATE * AUDIO_LOOPBACK_SECONDS)

/** @brief 扬声器预热静音时长（毫秒）。MAX98357 需要先收到时钟才使能输出 */
#define SPK_WARMUP_MS           100

/*==============================================================================
 * 播放命令
 *============================================================================*/
typedef enum {
    AUDIO_CMD_PLAY = 0,     // 播放音源
    AUDIO_CMD_STOP,         // 停止播放（并清空后续命令）
    AUDIO_CMD_RECORD_START, // 开始录音
    AUDIO_CMD_RECORD_STOP,  // 停止录音
    AUDIO_CMD_SYNTH_RELOAD, // ★ 重新初始化 KS 合成器（必须由音频任务执行）★
} App_Audio_Cmd_t;

/**
 * @brief 录音目标缓冲
 * @note  App_Audio_RecTarget_t 的定义在 app_audio.h（对外接口的一部分）。
 *        这里不再重复定义 —— 早期版本在 .c 里另起了一套 REC_TARGET_*
 *        和头文件里的对不上，是隐患，已合并。
 */

typedef struct {
    App_Audio_Cmd_t        cmd;
    const App_AudioSource_t *src;       // PLAY 用
    uint8_t                loop;        // PLAY 用：1 = 循环
    uint8_t                rewind;      // PLAY 用：1 = 播放前先重绕到开头
    App_Audio_RecTarget_t  target;      // RECORD_START / RECORD_STOP 用
    uint32_t               arg;         // RECORD_START 用（最长秒数）/ STOP 用（播放世代号）
    void                  *synth;       // SYNTH_RELOAD 用（ks_synth_t*，避免 APP_AUDIO 依赖 KS）
    void                  *synth_music; // SYNTH_RELOAD 用（music_t*）
    /**
     * @brief RECORD_START 用的录音世代号（只取低 8 位）
     * @note  ★ 为什么不复用 rewind 字段（审计复核建议）★
     *        原本把世代号塞进 `rewind` 里传 —— 功能上没出错（PLAY 和
     *        RECORD_START 是不同的 cmd，语义不重叠），但那是**隐式约定**：
     *        以后有人给 RECORD_START 加 rewind 语义就会踩坑。
     *        独立字段利用现有 padding，不增加结构体体积。
     */
    uint8_t                rec_gen;
} App_Audio_Msg_t;

/*==============================================================================
 * 静态状态
 *============================================================================*/
/** @brief 命令队列（按键任务 → 音频任务） */
static QueueHandle_t s_cmd_q = NULL;

/** @brief 录音缓冲（PSRAM）—— 普通录音，之后上传后端 */
static int16_t *s_rec_buf = NULL;
static size_t   s_rec_frames = 0;        // 已录到的样点数
static size_t   s_rec_capacity = 0;      // 容量（样点）
static volatile uint8_t s_recording = 0;
static volatile uint8_t s_rec_full = 0;

/** @brief 回环缓冲（PSRAM）—— 本地测试录音，不上传 */
static int16_t *s_loop_buf = NULL;
static size_t   s_loop_frames = 0;
static size_t   s_loop_capacity = 0;

/**
 * @brief 当前录音的上下文
 * @note  把「录到哪里、录多久、什么目标」集中在一处，
 *        避免早期那种"用容量隐式区分目标"的写法（那版是坏的）。
 *        只在音频任务里读写，无并发。
 */
static struct {
    App_Audio_RecTarget_t target;       // 录到哪个缓冲
    int16_t              *buf;          // 目标缓冲
    size_t               *frames;       // 目标缓冲的已录样点数
    size_t                capacity;     // 目标缓冲容量（样点）
    size_t                max_frames;   // 本次上限（达到即自动停止）
} s_rec_ctx = { AUDIO_REC_MAIN, NULL, NULL, 0, 0 };

/*==============================================================================
 * 内部：目标缓冲映射
 *============================================================================*/
/**
 * @brief 把「逻辑目标」映射成三个运行时字段
 * @param target 目标缓冲
 * @param buf    输出：缓冲指针（可为 NULL，表示只要 frames/capacity）
 * @param frames 输出：已录样点数指针
 * @param capacity 输出：容量（样点）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  ★ 集中在一处映射，避免每个函数各写一遍 if/else 而出错 ★
 */
static int audio_map_target(App_Audio_RecTarget_t target,
                            int16_t **buf, size_t **frames, size_t *capacity)
{
    switch (target)
    {
    case AUDIO_REC_MAIN:
        if (buf)      *buf      = s_rec_buf;
        if (frames)   *frames   = &s_rec_frames;
        if (capacity) *capacity = s_rec_capacity;
        return 0;

    case AUDIO_REC_LOOPBACK:
        if (buf)      *buf      = s_loop_buf;
        if (frames)   *frames   = &s_loop_frames;
        if (capacity) *capacity = s_loop_capacity;
        return 0;

    default:
        ESP_LOGE(TAG, "invalid record target %d", (int)target);
        return -1;
    }
}

/**
 * @brief 目标缓冲的名字（日志用）
 */
static const char *audio_target_name(App_Audio_RecTarget_t target)
{
    return (target == AUDIO_REC_LOOPBACK) ? "loopback" : "main";
}

/** @brief 当前播放状态 */
static const App_AudioSource_t *s_cur_src = NULL;
static uint8_t s_cur_loop = 0;
static volatile uint8_t s_playing = 0;

/** @brief 播放/录音共用的块缓冲（内部 RAM，摊平到静态区避免任务栈压力） */
static int16_t s_chunk[AUDIO_CHUNK_FRAMES];

/** @brief 录音时的临时读缓冲（32bit 槽 → 16bit 由 BSP 层完成，这里只要 16bit） */
static int16_t s_rec_chunk[512];

/** @brief 是否已初始化 */
static uint8_t s_inited = 0;

/*==============================================================================
 * ★ 录音共享状态的并发保护（审计后加固）★
 *============================================================================*/
/**
 * @brief 保护「已录帧数」和「正在泵数据」标志的自旋锁
 *
 * @note  ★ 为什么需要它（这是审计发现的真实数据竞争）★
 *
 *        录音的数据流是：**音频任务**往缓冲里写，**按键任务**在松手后把缓冲
 *        打包成 WAV 上传。链路是：
 *
 *          按键任务：App_Audio_RecordStop()   → 只入队一条停止命令就返回
 *                    key_do_upload_and_prepare()
 *                      → App_Audio_BuildWav()  → 立刻 memcpy 整块缓冲 ⚠
 *          音频任务：下一个循环才处理停止命令、停止写入
 *
 *        也就是说：**按键任务读缓冲的时候，音频任务可能正在往同一块缓冲 memcpy**，
 *        而且 `s_rec_frames` 也在被音频任务持续累加。两个后果：
 *          1) 上传的 WAV 尾部可能包含半写入的数据（杂音），或者长度与内容不一致
 *          2) `s_rec_frames` 的读发生在音频任务写它的同时 —— 非原子
 *
 *        另外 `s_recording` 原本被【两个任务】写：按键任务写 0（停止），
 *        音频任务录满时也写 0。这是明确的跨任务写同一变量。
 *
 *        修法（三件事）：
 *          a) 加 `s_rec_pumping` 标志：音频任务在写缓冲期间置 1，写完置 0
 *          b) `App_Audio_RecordStop()` 同步清 `s_recording`，然后【等】
 *             `s_rec_pumping` 变 0 才返回 —— 保证返回时缓冲已静止
 *          c) 帧数读取统一走 `audio_frames_atomic()`，锁内取一致的值
 *
 *        ★ 为什么用自旋锁而不是互斥量 ★
 *          临界区只有几条赋值，且按键任务等待时是"轮询 + 让出 CPU"，
 *          不需要内核阻塞语义。自旋锁更轻、也不会引入优先级反转。
 *
 *        ★ 纪律：临界区里不调任何可能阻塞/取其它锁的函数 ★
 *          所以 `audio_frames_atomic()` 只做纯读，不写 GPIO、不打日志。
 */
static portMUX_TYPE s_rec_mux = portMUX_INITIALIZER_UNLOCKED;

/*==============================================================================
 * ★ 播放/录音的世代号（审计修复 S2 / M5）★
 *============================================================================*/
/**
 * @brief 播放世代号
 *
 * @note  ★ 为什么不能用 xQueueReset 来实现"停止"★
 *
 *        `App_Audio_Stop()` 原来做的是「先 xQueueReset 清空整个命令队列，
 *        再压入 STOP」。但队列里可能正排着**别的**命令，最典型的是
 *        RECORD_START —— `xQueueReset` 会把它一起丢掉，导致：
 *
 *          1) 录音根本没开始，但上层以为在录（录音灯已点亮）
 *          2) 松手时 `s_recording` 还是 0，`App_Audio_RecordStop()` 走了
 *             "不是我在录"的早退分支 → **返回上一轮的帧数**
 *          3) 上层看到帧数 ≥ 0.5 秒 → 继续上传 → **上传的是上一轮的旧音频**
 *             （内容与用户刚说的话完全无关）
 *
 *        复现路径：KEY2 循环播放中 → 按 KEY1（入队 [STOP, REC_START]）
 *        → 250ms 内再按 KEY2 → 走"停止播放"分支 → xQueueReset 把
 *        [STOP, REC_START] 一起清掉。
 *
 *        ★ 修法 ★
 *          用世代号：`App_Audio_Stop()` 只递增这个号，然后在途的 PLAY 命令
 *          因为携带的是旧世代号而被音频任务丢弃。录音命令【永远不丢】。
 */
static volatile uint32_t s_play_gen = 0;

/**
 * @brief 录音请求世代号
 *
 * @note  ★ 解决"命令还在队列里就松手了"的问题 ★
 *
 *        短按 KEY1（按下到松手 < 20ms）时 RECORD_START 可能还没被音频任务
 *        取走，此时 `s_recording` 仍为 0，于是：
 *          · `App_Audio_RecordStop()` 的 `if (s_recording && ...)` 不成立
 *            → 不发 STOP
 *          · 音频任务随后处理 RECORD_START，开始录满 8 秒写进缓冲
 *          → 设备【实际一直在录音】，期间 KEY3 被"已经在录音中"拒绝
 *
 *        修法：**每次 Start 都递增**这个号。音频任务处理 RECORD_START 时
 *        先比对世代号，已经过期就直接跳过、根本不开始录。
 *        （因为本工程只有一个录音缓冲在录，"一个号"就够了，不需要每目标一个。）
 */
static volatile uint32_t s_rec_gen = 0;

/** @brief 音频任务处理 RECORD_START 时的世代号快照 */
static uint32_t s_rec_gen_at_start = 0;

/**
 * @brief 开始一个新的录音会话（使在途的录音命令失效）
 * @retval 新的世代号
 */
static uint32_t audio_new_rec_session(void)
{
    uint32_t g;

    /* 递增在临界区里：不是因为调用方并发（只有 Task_Key_Cmd 会调），
     * 而是因为【读侧在音频任务里】—— 锁保证它读到的是完整值而不是写了一半的。 */
    portENTER_CRITICAL(&s_rec_mux);
    g = ++s_rec_gen;
    portEXIT_CRITICAL(&s_rec_mux);

    return g;
}


/** @brief 音频任务是否正在往录音缓冲写数据（1 = 正在写） */
static volatile uint8_t s_rec_pumping = 0;

/*==============================================================================
 * ★ 合成器所有权：只允许音频任务碰它（审计修复的严重缺陷 S1）★
 *============================================================================*/
/**
 * @brief 合成器初始化/重绕的完成信号量
 * @note  ★ 为什么必须由音频任务来初始化合成器 ★
 *
 *        `KS_Synth_Init()` 第一件事是 `memset(s, 0, sizeof(*s))` —— 66KB 全部清零。
 *        而音频任务可能正在对**同一个对象**跑 `KS_Synth_Render()`：
 *          · memset 在 offset 0 就把 `music` 清成 NULL，
 *            而渲染路径 `ks_trigger_step()` 会用 `s->music->...` → **空指针解引用崩溃**
 *          · memset 推进到 `step_samples` 时它短暂为 0，
 *            而渲染里 `s->frame / s->step_samples` → **除零 panic**
 *
 *        触发路径不是理论（审计给出、我复核过）：
 *          ① 曾成功上传过一次（s_has_audio=1）
 *          ② 按 KEY1 开始录音，录音期间按一下 KEY2 → 走"有音频"分支，把 PLAY 入队
 *          ③ KEY1 松手 → 开始上传（原来这时才调 KS_Synth_Init）
 *          ④ 音频任务处理完录音后立刻继续 PLAY（原来 RECORD_START 不清 s_playing），
 *             于是**音乐在上传期间自己响**，同时合成器正被渲染
 *          ⑤ 上传结束时 KS_Synth_Init 去 memset 一个正在被渲染的对象 → 崩溃/爆音
 *
 *        修法：把「初始化合成器」变成一条**命令**，由音频任务执行；
 *        调用方通过这个信号量等回执。这样对象的所有权就唯一了。
 *
 *        ★ 为什么不用 s_led_mux 那种自旋锁 ★
 *          因为初始化要几毫秒、且要跨任务等待结果，自旋锁会把另一核烧住。
 */
static SemaphoreHandle_t s_synth_done = NULL;

/**
 * @brief 合成器会话锁（保证同一时刻只有一个调用方在等 SYNTH_RELOAD 的回执）
 * @note  探活/上传已经被 APP 层的 s_http_mux 串行化了，但那是 APP 层的约定。
 *        这里再加一道保险，避免将来有人在别的任务里调 App_Audio_SynthReload()。
 */
static SemaphoreHandle_t s_synth_lock = NULL;

/** @brief SYNTH_RELOAD 的结果（0 = 成功），由音频任务写、调用方读 */
static volatile int s_synth_result = -1;

/**
 * @brief 原子读取指定目标的已录帧数
 * @note  只读一个 size_t，锁内完成，不会读到"写了一半"的值。
 */
static size_t audio_frames_atomic(App_Audio_RecTarget_t target)
{
    size_t *frames = NULL;
    if (audio_map_target(target, NULL, &frames, NULL) != 0 || frames == NULL)
        return 0;

    portENTER_CRITICAL(&s_rec_mux);
    size_t n = *frames;
    portEXIT_CRITICAL(&s_rec_mux);
    return n;
}

/*==============================================================================
 * 线性缓冲音源
 *============================================================================*/
/**
 * @brief 线性音源的 fill 回调
 * @note  按 pos 顺序读，读到头返回 0（由播放任务决定是循环还是收尾）。
 */
static int audio_linear_fill(App_AudioSource_t *self, int16_t *out, size_t frames)
{
    App_AudioLinearSrc_t *s = (App_AudioLinearSrc_t *)self;

    if (s->pcm == NULL || s->pos >= s->frames)
        return -1;      /* 已结束 */

    size_t remain = s->frames - s->pos;
    size_t n = (frames < remain) ? frames : remain;

    memcpy(out, s->pcm + s->pos, n * sizeof(int16_t));
    s->pos += n;

    return (int)n;
}

/**
 * @brief 线性音源的 rewind 回调
 */
static void audio_linear_rewind(App_AudioSource_t *self)
{
    App_AudioLinearSrc_t *s = (App_AudioLinearSrc_t *)self;
    s->pos = 0;
}

void App_Audio_LinearSrcInit(App_AudioLinearSrc_t *s, const int16_t *pcm, size_t frames)
{
    if (s == NULL)
        return;

    memset(s, 0, sizeof(*s));
    s->base.fill    = audio_linear_fill;
    s->base.rewind  = audio_linear_rewind;
    s->base.user    = s;        /* 指向自己，便于回调里转换 */
    s->pcm          = pcm;
    s->frames       = frames;
    s->pos          = 0;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
int App_Audio_Init(void)
{
    if (s_inited)
        return 0;

    /* ---------- 1. I2S 双向通道 ---------- */
    if (BSP_I2S_Init() != 0)
    {
        ESP_LOGE(TAG, "BSP_I2S_Init failed");
        return -1;
    }

    /* 录音增益：默认 gain_shift = 16（不额外增益）
     * ★ 曾经默认 14（4 倍），但实测会在真实硬件上削顶（peak=1.0000/clipped=1），
     *   已改回 16。理由见 bsp_i2s.c 的 BSP_I2S_DEFAULT_GAIN_SHIFT 说明。 */
    BSP_I2S_SetGainShift(g_i2s_mic, AUDIO_REC_GAIN_SHIFT);
    BSP_I2S_SetVolume(g_i2s_spk, AUDIO_PLAYBACK_VOLUME);

    /* ---------- 2. 扬声器预热 ----------
     * MAX98357 上电后要先收到一段 BCLK/WS 才使能输出，
     * 否则第一个音符会被吃掉。这里送 100ms 静音"喂时钟"。 */
    int warm = BSP_I2S_WriteSilence(g_i2s_spk,
                                    (BSP_I2S_SAMPLE_RATE / 1000) * SPK_WARMUP_MS);
    ESP_LOGI(TAG, "speaker warmup: %d frames (~%d ms)", warm, SPK_WARMUP_MS);

    /* ---------- 3. 命令队列 ---------- */
    if (s_cmd_q == NULL)
    {
        s_cmd_q = xQueueCreate(AUDIO_CMD_QUEUE_LEN, sizeof(App_Audio_Msg_t));
        if (s_cmd_q == NULL)
        {
            ESP_LOGE(TAG, "create cmd queue failed");
            return -1;
        }
    }

    /* ---------- 3.5 合成器回执信号量（★ 曾经漏掉创建，是个 P0 bug ★）----------
     *
     * ★ 教训：这两个信号量一度只有声明和使用、没有创建 ★
     *   于是 App_Audio_SynthReload() 的 NULL 守卫恒真 → 立即 return -1
     *   → AUDIO_CMD_SYNTH_RELOAD 命令永远发不出去 → 音频任务里那个分支不可达
     *   → KS_Synth_Init 永不执行 → s_has_audio 永远为 0。
     *   用户症状是「上传明明 HTTP 200 成功，却报上传失败；之后 KEY2 永远没音频」——
     *   会被误判成网络问题（这个工程已经因为网络误判绕过两圈了）。
     *
     *   编译器不会报警：拿指针和 NULL 比较是合法 C。
     *   教训：**改完之后要回读文件确认改动真的落地了**，不能只看替换语句有没有报错。
     */
    if (s_synth_done == NULL)
    {
        s_synth_done = xSemaphoreCreateBinary();
        if (s_synth_done == NULL)
        {
            ESP_LOGE(TAG, "create synth-done semaphore failed");
            return -1;
        }
    }

    if (s_synth_lock == NULL)
    {
        /* ★ 用 Mutex 而不是 Binary ★
         *   binary 信号量的初始 count 是 0，创建后必须先 give 一次才能 take，
         *   否则首次 take 会白白等满 3 秒才失败（而且很容易漏掉那次 give）。
         *   mutex 创建出来就是"可用"状态，语义上也更贴切：它是用来
         *   串行化"合成器重载会话"的，不是用来传递事件的。 */
        s_synth_lock = xSemaphoreCreateMutex();
        if (s_synth_lock == NULL)
        {
            ESP_LOGE(TAG, "create synth-lock mutex failed");
            return -1;
        }
    }

    /* ---------- 4. 缓冲（PSRAM 优先） ---------- */
    size_t rec_bytes  = (size_t)AUDIO_REC_MAX_FRAMES * sizeof(int16_t);
    size_t loop_bytes = (size_t)AUDIO_LOOPBACK_MAX_FRAMES * sizeof(int16_t);

#if AUDIO_USE_PSRAM
    s_rec_buf  = (int16_t *)heap_caps_malloc(rec_bytes, MALLOC_CAP_SPIRAM);
    s_loop_buf = (int16_t *)heap_caps_malloc(loop_bytes, MALLOC_CAP_SPIRAM);
#endif

    if (s_rec_buf == NULL)
    {
        ESP_LOGW(TAG, "PSRAM alloc failed for record buffer, fallback to internal RAM");
        s_rec_buf = (int16_t *)heap_caps_malloc(rec_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_loop_buf == NULL)
    {
        ESP_LOGW(TAG, "PSRAM alloc failed for loopback buffer, fallback to internal RAM");
        s_loop_buf = (int16_t *)heap_caps_malloc(loop_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_rec_buf == NULL || s_loop_buf == NULL)
    {
        ESP_LOGE(TAG, "audio buffers alloc failed (rec=%p loop=%p)", s_rec_buf, s_loop_buf);
        return -1;
    }

    s_rec_capacity  = AUDIO_REC_MAX_FRAMES;
    s_loop_capacity = AUDIO_LOOPBACK_MAX_FRAMES;
    s_rec_frames    = 0;
    s_loop_frames   = 0;
    s_recording     = 0;
    s_rec_full      = 0;
    s_playing       = 0;
    s_cur_src       = NULL;


    s_inited = 1;

    ESP_LOGI(TAG, "init done: mic %u Hz, spk %u Hz, rec buf %u KB, loop buf %u KB (PSRAM)",
             BSP_I2S_SAMPLE_RATE, BSP_I2S_SAMPLE_RATE,
             (unsigned)(rec_bytes / 1024), (unsigned)(loop_bytes / 1024));
    ESP_LOGI(TAG, "  record: max %d s, loopback: %d s, playback chunk: %u frames (%u ms)",
             AUDIO_REC_MAX_SECONDS, AUDIO_LOOPBACK_SECONDS,
             (unsigned)AUDIO_CHUNK_FRAMES,
             (unsigned)(AUDIO_CHUNK_FRAMES * 1000 / BSP_I2S_SAMPLE_RATE));
    return 0;
}

/*==============================================================================
 * 播放控制（对外：只入队，不阻塞）
 *============================================================================*/
int App_Audio_Play(const App_AudioSource_t *src, uint8_t loop, uint8_t stop_current)
{
    if (src == NULL || src->fill == NULL || s_cmd_q == NULL)
        return -1;

    /* ★ 不在这里调 src->rewind()（审计后修正）★
     *
     * 原因：本函数由按键任务调用，而音频任务可能正在【同一个音源】上跑
     * fill() —— 两者都在读写音源的内部状态（线性音源的 pos、
     * KS 音源的 frame/voices）。原来的写法是明确的跨任务数据竞争：
     *   · 线性音源：按键任务把 pos 清 0，音频任务同时按旧 pos 拷贝 → 读错位置
     *   · KS 音源 ：rewind 会重置全部 32 个声部和 frame 计数器，
     *               而音频任务可能正在 ks_voice_next() 里用这些值 → 状态错乱
     *
     * 改成把"要重绕"作为一个标志随 PLAY 命令一起交给音频任务，
     * 由音频任务在切换音源的那一刻（此时它自己不会再并发跑 fill）执行。
     * 语义完全不变：仍然是"每次播放都从头开始"。 */

    /* ★ stop_current 用世代号实现，不能靠入队一条 STOP ——
     *   因为 STOP 排在新的 PLAY 后面才被处理就白搭，
     *   而且队列满时那条 STOP 会被丢掉。递增世代号能让所有在途 PLAY 立刻失效。 */
    if (stop_current)
        s_play_gen++;

    uint32_t gen = s_play_gen;      /* 本次 PLAY 属于这一代 */

    App_Audio_Msg_t msg = {
        .cmd    = AUDIO_CMD_PLAY,
        .src    = src,
        .loop   = loop,
        .rewind = (src->rewind != NULL) ? 1 : 0,   /* 交给音频任务执行 */
        .arg    = gen,
    };

    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "cmd queue full, play request dropped");
        return -1;
    }
    return 0;
}

int App_Audio_Stop(void)
{
    if (s_cmd_q == NULL)
        return -1;

    /* ★ 不再用 xQueueReset（审计修复 S2）★
     *
     * 原来这里的 xQueueReset 会清空【整个】命令队列，把排在前面的
     * RECORD_START / RECORD_STOP 一起丢掉 —— 后果是"录音没开始但上层以为在录"
     * 以及"松手时返回上一轮帧数 → 上传旧音频"。详见 s_play_gen 的注释。
     *
     * 现在改成：递增播放世代号，让所有【在途的 PLAY】失效；
     * 停止命令本身照常入队。录音类命令完全不受影响。 */
    s_play_gen++;

    App_Audio_Msg_t msg = {
        .cmd = AUDIO_CMD_STOP,
        .arg = s_play_gen,          /* 带上是哪一代的停止，便于日志排查 */
    };
    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE)
        return -1;
    return 0;
}

uint8_t App_Audio_IsPlaying(void)
{
    return s_playing;
}

uint8_t App_Audio_IsPlayingSource(const App_AudioSource_t *src)
{
    return (s_playing && s_cur_src == src) ? 1 : 0;
}


/*==============================================================================
 * 合成器所有权接口（★ 只允许音频任务碰合成器对象 ★）
 *============================================================================*/
/**
 * @brief 请求音频任务重新初始化合成器，并阻塞等回执
 * @param synth ks_synth_t*（用 void* 传，避免 APP_AUDIO 组件依赖 KS 组件）
 * @param music music_t*
 * @retval  0: 音频任务已完成初始化
 * @retval -1: 参数错误 / 队列满 / 等回执超时
 *
 * @note  ★★ 这是审计发现的严重缺陷 S1 的修复 ★★
 *
 *        原来 `KS_Synth_Init()` 直接在按键任务里调用，而它的第一件事是
 *        `memset(s, 0, sizeof(*s))` —— 把 66KB 的合成器对象整个清零。
 *        与此同时音频任务可能正在对**同一个对象**跑 `KS_Synth_Render()`：
 *          · memset 在 offset 0 就把 `music` 清成 NULL，
 *            而渲染路径会解引用 `s->music->...` → **空指针 LoadProhibited 崩溃**
 *          · memset 推进到 `step_samples` 时它短暂为 0，
 *            而渲染里 `s->frame / s->step_samples` → **除零 panic**
 *
 *        触发路径真实可达（审计给出、我复核过）：
 *          ① 曾成功上传过一次（s_has_audio = 1）
 *          ② 按 KEY1 开始录音；录音期间按一下 KEY2 → 走"有音频"分支把 PLAY 入队
 *          ③ KEY1 松手 → 开始上传（原来在这一步才调 KS_Synth_Init）
 *          ④ 音频任务处理完录音后立刻继续 PLAY（因为 RECORD_START 原先不清
 *             s_playing）→ **音乐在上传期间自己响**，合成器正在被渲染
 *          ⑤ 上传结束 → KS_Synth_Init 去 memset 一个正在被渲染的对象 → 崩溃/爆音
 *
 *        修法：把「初始化合成器」变成一条**命令**，由音频任务执行，
 *        调用方通过信号量等回执。这样合成器对象的所有权就是唯一的。
 *
 *        ★ 为什么不用自旋锁 ★
 *          初始化要几毫秒且需要跨任务等待结果，自旋锁会把另一个核心烧住；
 *          信号量会让出 CPU，是正确选择。
 */
int App_Audio_SynthReload(void *synth, void *music)
{
    if (synth == NULL || music == NULL || s_cmd_q == NULL ||
        s_synth_done == NULL || s_synth_lock == NULL)
        return -1;

    if (xSemaphoreTake(s_synth_lock, pdMS_TO_TICKS(3000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "synth reload: 等会话锁超时");
        return -1;
    }

    int ret = -1;

    do
    {
        App_Audio_Msg_t msg = {
            .cmd         = AUDIO_CMD_SYNTH_RELOAD,
            .synth       = synth,
            .synth_music = music,
        };

        if (xQueueSend(s_cmd_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGE(TAG, "synth reload: 命令队列满");
            break;
        }

        /* 等音频任务回执：只等一次 memset(66KB) + 参数换算，通常几毫秒 */
        if (xSemaphoreTake(s_synth_done, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            ESP_LOGE(TAG, "synth reload: 等回执超时（音频任务没响应？）");
            break;
        }

        ret = s_synth_result;

    } while (0);

    xSemaphoreGive(s_synth_lock);
    return ret;
}

/*==============================================================================
 * 录音控制（对外：只入队 / 只查询）
 *============================================================================*/
/**
 * @brief 内部：请求开始录音
 * @param target      录到哪个缓冲
 * @param max_seconds 最长秒数
 * @retval 0: 已入队
 * @retval -1: 失败
 */
static int audio_request_record(App_Audio_RecTarget_t target, uint32_t max_seconds)
{
    if (s_cmd_q == NULL)
        return -1;

    /* ★ 不再 xQueueReset（审计修复 S2）★
     * 原来这里也用 xQueueReset 清队列，同样会把在途的 RECORD_STOP
     * 或另一个目标的 RECORD_START 丢掉。改成用播放世代号作废在途 PLAY，
     * 录音命令一律不丢。 */
    s_play_gen++;

    App_Audio_Msg_t stop = {
        .cmd = AUDIO_CMD_STOP,
        .arg = s_play_gen,
    };
    xQueueSend(s_cmd_q, &stop, 0);

    /* ★ 录音世代号：让先前在途的 RECORD_START / RECORD_STOP 失效 ★
     *   短按 KEY1 时 START 可能还在队列里，松手后就会白录 8 秒；
     *   有了世代号，音频任务取出 START 时会发现它已过期，直接跳过。 */
    uint32_t gen = audio_new_rec_session();

    App_Audio_Msg_t msg = {
        .cmd     = AUDIO_CMD_RECORD_START,
        .target  = target,
        .arg     = max_seconds,
        /* ★ 用独立字段 rec_gen 传世代号（审计复核建议）★
         * 原来借用 rewind 字段：功能上没出错（PLAY 和 RECORD_START 是
         * 不同的 cmd，语义不重叠），但那是隐式约定，容易踩坑。
         * 独立字段利用现有 padding，不增加结构体体积。
         * （arg 已被 max_seconds 占用，所以不能塞进 arg。） */
        .rec_gen = (uint8_t)(gen & 0xFF),
    };

    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "cmd queue full, record request dropped");
        return -1;
    }
    return 0;
}

int App_Audio_RecordStart(App_Audio_RecTarget_t target, uint32_t max_seconds)
{
    /* 各自的上限不同：主缓冲 8 秒（要上传，后端限 15 秒），回环缓冲 5 秒 */
    uint32_t cap_sec = (target == AUDIO_REC_LOOPBACK)
                       ? AUDIO_LOOPBACK_SECONDS : AUDIO_REC_MAX_SECONDS;

    if (max_seconds == 0 || max_seconds > cap_sec)
        max_seconds = cap_sec;

    ESP_LOGI(TAG, "request record: %s buffer, max %u s",
             audio_target_name(target), (unsigned)max_seconds);
    return audio_request_record(target, max_seconds);
}

int App_Audio_RecordStop(App_Audio_RecTarget_t target)
{
    size_t *frames = NULL;

    if (audio_map_target(target, NULL, &frames, NULL) != 0)
        return 0;

    /* ★ 只停"正在录的那个目标" ★
     * 如果当前录的是别的缓冲，发 STOP 会误停别人，所以先判断。 */
    if (!s_recording || s_rec_ctx.target != target)
        return (int)audio_frames_atomic(target);

    /* ---------- 1. 同步清标志 ----------
     * ★ 关键：不能只入队停止命令就返回 ★
     *   音频任务要下一个循环才处理那条命令，而调用方（按键任务）会立刻
     *   去读录音缓冲打包 WAV —— 那时音频任务可能还在往同一块缓冲 memcpy。
     *   所以这里【直接清 s_recording】，让音频任务在本轮循环结束时立刻停止写入。
     *   入队的 STOP 命令仍然保留，负责打日志和收尾。 */
    s_recording = 0;

    if (s_cmd_q != NULL)
    {
        App_Audio_Msg_t msg = { .cmd = AUDIO_CMD_RECORD_STOP, .target = target };
        xQueueSend(s_cmd_q, &msg, 0);
    }

    /* ---------- 2. 等音频任务把"正在泵"的那一块写完 ----------
     * 音频任务在 memcpy 期间把 s_rec_pumping 置 1、写完置 0。
     * 最长只需等一个音频任务循环（≤ 20ms + 一次 I2S 读），这里给 100ms 上限兜底，
     * 每 1ms 让出一次 CPU（不能用 vTaskDelay(1) 死等，那会拖慢；
     * 也不自旋空转，那会白烧 CPU）。 */
    for (int i = 0; i < 100 && s_rec_pumping; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (s_rec_pumping)
    {
        ESP_LOGW(TAG, "等待录音泵结束超时（%s 缓冲），帧数可能略有偏差",
                 audio_target_name(target));
    }

    /* ---------- 3. 回到锁里取一致的长度 ---------- */
    return (int)audio_frames_atomic(target);
}

uint8_t App_Audio_IsRecordingTarget(App_Audio_RecTarget_t target)
{
    return (s_recording && s_rec_ctx.target == target) ? 1 : 0;
}

uint8_t App_Audio_IsRecording(void)
{
    return s_recording;
}

uint8_t App_Audio_RecordIsFull(void)
{
    return s_rec_full;
}

const int16_t *App_Audio_GetRecordPcm(App_Audio_RecTarget_t target)
{
    int16_t *buf = NULL;
    if (audio_map_target(target, &buf, NULL, NULL) != 0)
        return NULL;
    return buf;
}

/**
 * @brief 查询指定缓冲已录到的样点数
 * @note  ★ 走原子读 ★
 *        音频任务在持续累加这个值，直接解引用可能读到写了一半的 size_t。
 *        （ESP32-S3 上 32 位对齐读本身是原子的，但加锁能保证读到的是
 *         "和缓冲内容一致"的值，而不是"刚写进去但 memcpy 还没做完"的值。）
 */
size_t App_Audio_GetRecordFrames(App_Audio_RecTarget_t target)
{
    return audio_frames_atomic(target);
}

/*==============================================================================
 * 录音质量诊断（回环测试靠串口判断麦克风好坏，核心就是这个函数）
 *============================================================================*/
int App_Audio_GetRecStats(App_Audio_RecTarget_t target, App_Audio_RecStats_t *st)
{
    int16_t *buf = NULL;

    if (st == NULL || audio_map_target(target, &buf, NULL, NULL) != 0)
        return -1;

    memset(st, 0, sizeof(*st));

    /* ★ 前置条件：调用方必须先 App_Audio_RecordStop() ★
     *   本函数会读 PCM 缓冲的全部内容做统计，如果录音还在进行（或音频任务
     *   正把最后一块写进去），统计结果就会包含半写入的数据。
     *   RecordStop 内部已经保证了"返回时缓冲已静止"，所以只要遵守调用顺序就安全。 */
    size_t n = audio_frames_atomic(target);
    st->frames = (uint32_t)n;

    /* ★ 0 帧也要返回成功（不是错误）★
     * 因为"录到 0 帧"正是最有价值的诊断结果：麦克风一个样点都没给，
     * 问题在接线/供电，不在代码。返回错误反而会让调用方误以为是软件 bug。 */
    if (n == 0 || buf == NULL)
    {
        st->ms = 0;
        return 0;
    }

    st->ms = (uint32_t)((uint64_t)n * 1000ULL / BSP_I2S_SAMPLE_RATE);

    double sum = 0.0, sum_sq = 0.0;
    int32_t peak_abs = 0;
    uint8_t clipped = 0;

    for (size_t i = 0; i < n; i++)
    {
        int32_t v = buf[i];
        int32_t a = (v < 0) ? -v : v;

        sum    += (double)v;
        sum_sq += (double)v * (double)v;
        if (a > peak_abs) peak_abs = a;
        if (a >= 32767) clipped = 1;
    }

    st->rms     = (float)(sqrt(sum_sq / (double)n) / 32768.0);
    st->peak    = (float)((double)peak_abs / 32768.0);
    st->dc      = (float)((sum / (double)n) / 32768.0);
    st->clipped = clipped;

    return 0;
}

int App_Audio_BuildWav(App_Audio_RecTarget_t target, uint8_t *out, size_t out_cap)
{
    int16_t *buf = NULL;

    if (out == NULL || audio_map_target(target, &buf, NULL, NULL) != 0)
        return 0;

    /* 同上：调用方必须先 RecordStop，否则可能打包进半写入的数据 */
    size_t n = audio_frames_atomic(target);
    if (buf == NULL || n == 0)
        return 0;

    size_t data_bytes = n * sizeof(int16_t);
    size_t total = BSP_I2S_WAV_HEADER_SIZE + data_bytes;

    if (out_cap < total)
    {
        ESP_LOGE(TAG, "wav buffer too small: need %u, have %u",
                 (unsigned)total, (unsigned)out_cap);
        return -1;
    }

    /* ★ 头必须由 BSP 层生成，格式对齐后端契约：
     *   RIFF/WAVE、PCM(audioFormat=1)、单声道、16000Hz、16bit ★ */
    if (BSP_I2S_BuildWavHeader(out, (uint32_t)data_bytes, BSP_I2S_SAMPLE_RATE) != 0)
        return -1;

    memcpy(out + BSP_I2S_WAV_HEADER_SIZE, buf, data_bytes);

    return (int)total;
}

/*==============================================================================
 * 录音音源（供播放用）
 *============================================================================*/
/**
 * @brief 音源实例：两个目标各一个（不能用同一个，否则切换目标时状态会串）
 * @note  线性音源结构体很小，静态分配即可。
 */
static App_AudioLinearSrc_t s_src_main;
static App_AudioLinearSrc_t s_src_loop;

const App_AudioSource_t *App_Audio_GetRecSrc(App_Audio_RecTarget_t target)
{
    int16_t *buf = NULL;

    if (audio_map_target(target, &buf, NULL, NULL) != 0)
        return NULL;

    size_t n = App_Audio_GetRecordFrames(target);
    if (buf == NULL || n == 0)
        return NULL;

    App_AudioLinearSrc_t *s = (target == AUDIO_REC_LOOPBACK) ? &s_src_loop : &s_src_main;

    /* 每次取都重新初始化：长度和位置都可能变（重新录了一轮） */
    App_Audio_LinearSrcInit(s, buf, n);
    return &s->base;
}

/*==============================================================================
 * 回环测试便捷封装
 *============================================================================*/
int App_Audio_LoopbackRecordStart(uint32_t max_seconds)
{
    return App_Audio_RecordStart(AUDIO_REC_LOOPBACK, max_seconds);
}

int App_Audio_LoopbackRecordStop(void)
{
    return App_Audio_RecordStop(AUDIO_REC_LOOPBACK);
}

uint8_t App_Audio_HasLoopback(void)
{
    return (s_loop_frames > 0) ? 1 : 0;
}

const App_AudioSource_t *App_Audio_GetLoopbackSrc(void)
{
    return App_Audio_GetRecSrc(AUDIO_REC_LOOPBACK);
}

size_t App_Audio_GetLoopbackFrames(void)
{
    return s_loop_frames;
}

/*==============================================================================
 * 状态诊断
 *============================================================================*/
void App_Audio_DumpStatus(void)
{
    ESP_LOGI(TAG, "---- audio status ----");
    ESP_LOGI(TAG, "  inited=%u  playing=%u  recording=%u  rec_full=%u",
             s_inited, s_playing, s_recording, s_rec_full);
    ESP_LOGI(TAG, "  main record : %u frames (%.1f s) / capacity %u frames",
             (unsigned)s_rec_frames,
             (double)s_rec_frames / (double)BSP_I2S_SAMPLE_RATE,
             (unsigned)s_rec_capacity);
    ESP_LOGI(TAG, "  loopback    : %u frames (%.1f s) / capacity %u frames",
             (unsigned)s_loop_frames,
             (double)s_loop_frames / (double)BSP_I2S_SAMPLE_RATE,
             (unsigned)s_loop_capacity);
    ESP_LOGI(TAG, "  heap: internal %u B, PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "----------------------");
}

const char *App_Audio_RetStr(int ret)
{
    if (ret > 0)  return "OK(frames)";
    if (ret == 0) return "EMPTY";
    if (ret == -1) return "FAIL";
    return "UNKNOWN";
}

/*==============================================================================
 * 内部：录音一轮
 *============================================================================*/
/**
 * @brief 按当前录音上下文采集一块数据
 * @note  ★ 用非阻塞读（timeout=0）★
 *        音频任务还要兼顾播放推进，不能在这里长时间阻塞。
 *        每轮最多读 512 个样点，靠 App_Audio_Service 的循环频率累积。
 */
static void audio_do_record(void)
{
    if (s_rec_ctx.buf == NULL || s_rec_ctx.frames == NULL)
        return;

    /* I2S 读放在锁外：它可能阻塞、也可能取内部锁，绝不能放进自旋锁里 */
    int n = BSP_I2S_Read(g_i2s_mic, s_rec_chunk, ARRAY_SIZE(s_rec_chunk), 0);
    if (n <= 0)
        return;     /* 还没数据，下一轮再来 */

    /* ★★ 进锁 + 置"正在泵" + 复核 s_recording（审计修复 M8 的 TOCTOU）★★
     *
     * 原来的顺序是「锁外 s_rec_pumping = 1 → 进锁 → 写缓冲」，有两个漏洞：
     *
     *  1) 标志设置在锁外，而复核 s_recording 是在【更早】的调用点做的
     *     （App_Audio_Service 主循环里）。如果音频任务在 I2S 读那里卡了一下
     *     （读最多阻塞一个 chunk 的时间），期间 RecordStop 清了 s_recording
     *     并看到一个还是 0 的 s_rec_pumping → 它认为"缓冲已静止"就返回了，
     *     紧接着调用方开始 memcpy 打包 WAV，而这边恢复后又追加了一块
     *     → **WAV 尾部半写入、声明长度与实际内容不一致**。
     *
     *  2) 即使标志置得够早，锁内没有复核 s_recording，所以"已经被命令停止"
     *     这件事拦不住这次写入。
     *
     * 修法：把「置标志」和「复核」都放进临界区。临界区里一旦发现
     * s_recording 已经是 0，就直接丢弃这批数据（它本来也不该再进缓冲）。
     * 这样 RecordStop 只要看到 s_rec_pumping == 0（且它自己也清了
     * s_recording），就能确定缓冲不会再被写了 —— 等待才有意义。 */
    portENTER_CRITICAL(&s_rec_mux);

    if (!s_recording)
    {
        /* 已经被停止（或已被更晚的录音会话取代）→ 这批数据不要了 */
        portEXIT_CRITICAL(&s_rec_mux);
        return;
    }

    /* 在这里置标志：进了锁就说明"从现在到出锁，缓冲归我写" */
    s_rec_pumping = 1;

    size_t done = *s_rec_ctx.frames;
    size_t room = (s_rec_ctx.max_frames > done) ? (s_rec_ctx.max_frames - done) : 0;
    size_t take = ((size_t)n < room) ? (size_t)n : room;

    if (take > 0)
    {
        /* memcpy 在临界区里 —— 会被自旋锁保护，期间按键任务读不到中间态。
         * 块很小（≤512 样点 = 1KB），拷贝耗时可忽略，不会让另一核等太久。
         * （这块缓冲在 PSRAM，拷 1KB 约十几微秒；自旋锁期间关的是本核中断，
         *   另一核只是短暂自旋，不构成可观测影响。） */
        memcpy(s_rec_ctx.buf + done, s_rec_chunk, take * sizeof(int16_t));
        *s_rec_ctx.frames = done + take;
    }

    size_t total = *s_rec_ctx.frames;
    uint8_t full = (total >= s_rec_ctx.max_frames) ? 1 : 0;

    /* 录满就自动停止（安全兜底：防止按住不放把缓冲写满）。
     * ★ 这里写 s_recording 是在临界区里，和 App_Audio_RecordStop 的写互斥 ★ */
    if (full)
    {
        s_recording = 0;
        s_rec_full  = 1;
    }

    /* 标志在锁内清掉：保证"看到 pumping==0"一定意味着缓冲已经写完了 */
    s_rec_pumping = 0;

    portEXIT_CRITICAL(&s_rec_mux);

    if (full)
    {
        ESP_LOGW(TAG, "%s 录音达上限自动停止（%u 帧, %.1f s）",
                 audio_target_name(s_rec_ctx.target),
                 (unsigned)total,
                 (double)total / (double)BSP_I2S_SAMPLE_RATE);
    }
}

/*==============================================================================
 * 周期服务：音频任务主体
 *============================================================================*/
void App_Audio_Service(uint32_t timeout_ms)
{
    if (!s_inited)
        return;

    /* ---------- 1. 看有没有命令 ---------- */
    App_Audio_Msg_t msg;
    if (xQueueReceive(s_cmd_q, &msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        switch (msg.cmd)
        {
        case AUDIO_CMD_STOP:
            /* 停止永远受理（它本来就是"让现在别响了"），
             * 世代号只用于日志追踪，不做丢弃判断 —— 否则快速连按会漏停。 */
            s_cur_src = NULL;
            s_playing = 0;
            /* 送一小段静音，把 DMA 里的残留数据冲掉，
             * 否则停止后还会"漏"出几十毫秒的旧音频 */
            BSP_I2S_WriteSilence(g_i2s_spk, 256);
            ESP_LOGI(TAG, "playback stopped");
            break;

        case AUDIO_CMD_PLAY:
            /* ★ 丢弃过期的播放请求（审计修复 S2）★
             * 如果这条 PLAY 发出之后有人调过 App_Audio_Stop() 或又发起了
             * 新的录音/播放，世代号就变了 —— 那说明这条请求已经不该生效。
             * 这正是取代 xQueueReset 的机制：丢的是"过期请求"，
             * 而不是"队列里所有命令"（后者会把 RECORD_START 一起丢掉）。 */
            if (msg.arg != s_play_gen)
            {
                ESP_LOGD(TAG, "丢弃过期 PLAY（世代 %u != 当前 %u）",
                         (unsigned)msg.arg, (unsigned)s_play_gen);
                break;
            }

            /* ★ 在音频任务里执行重绕 ★
             * 此时还没开始对新的 src 调 fill()，而且旧的播放已经在这一刻被替换，
             * 所以不存在"和 fill 并发改音源状态"的问题。 */
            if (msg.rewind && msg.src != NULL && msg.src->rewind != NULL)
                msg.src->rewind((App_AudioSource_t *)msg.src);

            s_cur_src  = msg.src;
            s_cur_loop = msg.loop;
            s_playing  = 1;
            ESP_LOGI(TAG, "play start (loop=%u, rewind=%u)", msg.loop, msg.rewind);
            break;

        case AUDIO_CMD_SYNTH_RELOAD:
        {
            /* ★ 全工程只有这里可以调 KS_Synth_Init ★
             *   音频任务独占合成器对象，理由见 App_Audio_SynthReload() 的注释。
             *   用 extern 声明而不是 include ks_synth.h：保持 APP_AUDIO 不依赖
             *   Middlewares 组件（分层约定），参数用 void* 传递。 */
            extern int KS_Synth_Init(void *s, const void *music, uint32_t sample_rate);

            s_synth_result = KS_Synth_Init(msg.synth, msg.synth_music,
                                           (uint32_t)BSP_I2S_SAMPLE_RATE);

            if (s_synth_result == 0)
                ESP_LOGI(TAG, "合成器已在音频任务里重新初始化完成");
            else
                ESP_LOGE(TAG, "合成器重新初始化失败 (%d)", s_synth_result);

            xSemaphoreGive(s_synth_done);      /* 回执给调用方 */
            break;
        }

        case AUDIO_CMD_RECORD_START:
        {
            /* ★ 丢弃过期的录音请求（审计修复 S2 的另一半）★
             * 短按 KEY1 时这条 START 可能还在队列里，用户已经松手了。
             * 若不管它就照录 8 秒 → 设备"幽灵录音"：
             * 界面显示没在录、实际一直在录，期间 KEY3 还会被拒绝。 */
            if ((uint8_t)(s_rec_gen & 0xFF) != msg.rec_gen)
            {
                ESP_LOGI(TAG, "丢弃过期 RECORD_START（用户已松手）");
                break;
            }

            int16_t *buf = NULL;
            size_t  *frames = NULL;
            size_t   capacity = 0;

            if (audio_map_target(msg.target, &buf, &frames, &capacity) != 0)
                break;

            /* 两个缓冲的上限不同：主缓冲 8 秒（要上传），回环缓冲 5 秒 */
            uint32_t cap_sec = (msg.target == AUDIO_REC_LOOPBACK)
                               ? AUDIO_LOOPBACK_SECONDS : AUDIO_REC_MAX_SECONDS;
            uint32_t max_sec = (msg.arg == 0 || msg.arg > cap_sec) ? cap_sec : msg.arg;

            s_rec_ctx.target     = msg.target;
            s_rec_ctx.buf        = buf;
            s_rec_ctx.frames     = frames;
            s_rec_ctx.capacity   = capacity;
            s_rec_ctx.max_frames = (size_t)BSP_I2S_SAMPLE_RATE * max_sec;
            if (s_rec_ctx.max_frames > capacity)
                s_rec_ctx.max_frames = capacity;

            /* ★ 先把 RX DMA 里的陈数据抽空（审计修复 M10）★
             * 空闲时麦克风 DMA 一直在跑但没人读，环里是"按键之前"的旧数据。
             * 不清掉的话，录音头 ~90ms 是旧声音，再加上命令处理延迟，
             * 用户按下 KEY1 后最先 0.1~0.35 秒的语音就采不到 ——
             * 送 Whisper 的开头被截会明显拉低识别率。
             * 这里用非阻塞读把已有的数据全部丢弃，从"最新"开始录。 */
            {
                int16_t junk[256];
                while (BSP_I2S_Read(g_i2s_mic, junk, ARRAY_SIZE(junk), 0) > 0)
                {
                    /* 丢弃：目的就是把这批陈数据倒掉 */
                }
            }

            /* ★ 发布录音状态（加锁，与 audio_frames_atomic 的读侧对称）★
             * 原来这里的 *frames = 0 和 s_recording = 1 都没有进临界区，
             * 而读侧 audio_frames_atomic() 是加锁读的 —— 锁只覆盖一边等于没保护。 */
            portENTER_CRITICAL(&s_rec_mux);

            if (frames != NULL)
                *frames = 0;        /* 从 0 开始覆盖上一轮录音 */

            s_rec_full  = 0;
            s_recording = 1;
            s_rec_gen_at_start = s_rec_gen;

            portEXIT_CRITICAL(&s_rec_mux);

            /* ★ 清播放状态（审计修复 M5）★
             * 原来 RECORD_START 从不碰 s_playing，于是录音期间
             * App_Audio_IsPlaying() 仍返回 1（KEY2 行为错乱），
             * 而且录音一停就【自动恢复播放】—— 用户没按播放键音乐自己响。 */
            s_cur_src = NULL;
            s_playing = 0;

            ESP_LOGI(TAG, "录音开始：%s 缓冲，上限 %u 帧 (%.1f s)",
                     audio_target_name(msg.target),
                     (unsigned)s_rec_ctx.max_frames,
                     (double)s_rec_ctx.max_frames / (double)BSP_I2S_SAMPLE_RATE);
            break;
        }

        case AUDIO_CMD_RECORD_STOP:
            /* 只停“目标匹配”的那一路，否则 KEY1 松手会把 KEY3 的回环录音一起停掉 */
            if (s_recording && msg.target == s_rec_ctx.target)
            {
                s_recording = 0;
                ESP_LOGI(TAG, "录音停止：%s 缓冲，%u 帧 (%.1f s)",
                         audio_target_name(s_rec_ctx.target),
                         (unsigned)((s_rec_ctx.frames != NULL) ? *s_rec_ctx.frames : 0),
                         (double)((s_rec_ctx.frames != NULL) ? *s_rec_ctx.frames : 0)
                             / (double)BSP_I2S_SAMPLE_RATE);
            }
            break;
        default:
            break;
        }
    }

    /* ---------- 2. 录音优先 ---------- */
    /* ★ 录音优先于播放：录音是"一次性、不可重来"的输入，
     *   丢一个样点就永久丢了；播放可以稍等。 */
    if (s_recording)
    {
        audio_do_record();
        return;     /* 录音期间不播放，避免 I2S 互相干扰 */
    }

    /* ---------- 3. 播放推进 ---------- */
    if (!s_playing || s_cur_src == NULL || s_cur_src->fill == NULL)
        return;

    int got = s_cur_src->fill((App_AudioSource_t *)s_cur_src, s_chunk, AUDIO_CHUNK_FRAMES);

    if (got > 0)
    {
        int written = BSP_I2S_Write(g_i2s_spk, s_chunk, (size_t)got, 0);
        if (written < 0)
            ESP_LOGW(TAG, "i2s write failed");
        return;
    }

    /* got <= 0：一轮结束 */
    if (got == 0)
        return;     /* 流式音源暂时没数据，下轮再试 */

    if (s_cur_loop)
    {
        /* ★ 循环：重置音源再继续 ★
         * 需求里的「KEY2 循环播放，再按停止后下次从头开始」靠这里 + Rewind 实现。 */
        if (s_cur_src->rewind != NULL)
            s_cur_src->rewind((App_AudioSource_t *)s_cur_src);
        ESP_LOGD(TAG, "loop restart");
    }
    else
    {
        s_playing = 0;
        ESP_LOGI(TAG, "playback finished (loop disabled)");
        /* 播完后送静音冲掉 DMA 残留，避免尾音重复 */
        BSP_I2S_WriteSilence(g_i2s_spk, 256);
    }
}
