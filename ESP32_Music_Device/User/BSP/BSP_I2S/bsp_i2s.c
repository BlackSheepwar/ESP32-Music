/**
 * @file bsp_i2s.c
 * @brief I2S 音频 BSP 层实现（句柄式接口 + 内置硬件映射表）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 映射表把「端口 + 引脚 + 采样率 + 方向」写死在本文件，同时兼作句柄表。
 *
 *==============================================================================
 * ★ 硬件接线（详见工程根目录 HARDWARE.md）★
 *==============================================================================
 *
 *  INMP441（I2S MEMS 麦克风，只收）→ ESP32-S3，挂在 I2S_NUM_0：
 *    VDD  → 3V3          （★ 务必并联 100nF + 10uF 到 GND，纹波会让底噪变大）
 *    GND  → GND
 *    L/R  → GND          （★ 接地 = 左声道输出，与本文件 slot_mask = LEFT 对应）
 *    WS   → GPIO42
 *    SCK  → GPIO41
 *    SD   → GPIO47       （ESP32 输入）
 *
 *  MAX98357A（I2S 功放，只发）→ ESP32-S3，挂在 I2S_NUM_1：
 *    VIN  → 5V 或 3V3    （★ 3V3 供电时输出功率小，建议 5V；模块带 3.3V 稳压）
 *    GND  → GND
 *    LRC  → GPIO46
 *    BCLK → GPIO3
 *    DIN  → GPIO2        （ESP32 输出）
 *    GAIN → 悬空 = 9dB；接 GND = 15dB；接 VDD = 3dB；经 100k 接 GND = 6dB
 *    SD   → ★★ 必须显式接到 VIN（或 3V3）★★  拉低 = 关断输出
 *    SPK+ / SPK- → 扬声器（★ 是 BTL 差分输出，两端都不能接地！）
 *
 *  ★★ SD 脚不能悬空（实测踩过的坑，2026-10）★★
 *
 *    原厂 MAX98357A 的 datasheet 确实说 SD 悬空时靠【内部上拉】使能，
 *    所以"悬空"在纸面上是对的。但**常见廉价仿制模块省略了这个内部上拉**，
 *    于是引脚一直浮空在负电压附近（实测 −0.23V）→ 模块始终处于关断 →
 *    **完全没声音，而软件侧一切正常**（日志显示 playback finished，
 *    数据确实写进了 I2S），极难定位。
 *
 *    真机现象：
 *      SD 悬空   → SD 实测 −0.23V，SPK+ 实测 −0.2V（应 ≈VIN/2 = 2.5V），没声
 *      SD 接 VIN → SPK+ 立刻回到 ≈2.5V，出声 ✅
 *
 *    ★ 结论：显式拉高在【原厂和仿制模块上都正确】，悬空只在原厂上碰运气。
 *      凡是"靠器件内部默认值"的接法，都应当改成显式连接 —— 生产环境尤其如此。
 *
 *  ★ I2S 不需要 MCLK：INMP441 和 MAX98357 内部都有 PLL，只用 BCLK/WS 即可。
 */

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "ds_common.h"
#include "bsp_i2s.h"
#include "bsp_sys.h"     /* BSP_SYS_GetTickMs */
// 功能包含
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "BSP_I2S";

/** @brief 录音默认数字增益右移位数
 *
 *  ★ 已按实测从 14 改成 16（不再额外增益）★
 *
 *  14 是一开始的保守估计：担心 INMP441 电平偏低（灵敏度约 -26dBFS），
 *  所以多给了 4 倍数字增益（>>14 相当于 >>16 之后再 x4）。
 *
 *  实测（2026-10，你的硬件）证明这个估计偏保守了：
 *      frames=2048  rms=0.2318  peak=1.0000  dc= 0.0127  clipped=1
 *      frames=1536  rms=0.1909  peak=1.0000  dc=-0.0040  clipped=1
 *  peak 顶到 1.0000 且 clipped=1 = 【削顶】：波形被切平，
 *  谐波失真会拉低 Whisper 识别率，听起来也会"破"。
 *
 *  16 = 只取高 16 位、不做额外增益，直接反映 INMP441 的原始电平。
 *  ★ 换别的 INMP441 模块后若发现 rms 偏小（< 0.01），
 *    再改回 15（2 倍）或 14（4 倍）即可 —— 这个值只影响录音，不影响播放。 */
#define BSP_I2S_DEFAULT_GAIN_SHIFT  16

/** @brief DMA 描述符数量（单缓冲帧数 240，6 个缓冲 = 1440 帧 = 90ms @16kHz，足够扛住 WiFi 抖动） */
#define BSP_I2S_DMA_DESC_NUM        6
/** @brief 单个 DMA 缓冲的帧数（必须是 4 的倍数，32bit 槽下即 16 字节对齐） */
#define BSP_I2S_DMA_FRAME_NUM       240

/** @brief 把 int32 限幅到 int16 范围 */
#define CLAMP_I16(v)    ((v) > 32767 ? 32767 : ((v) < -32768 ? -32768 : (v)))

/*==============================================================================
 * 类型定义与硬件映射表
 *============================================================================*/
/**
 * @brief I2S 句柄结构体定义（对上层不透明）
 */
struct BSP_I2S_Handle {
    /* ---- 静态配置（构造后不再改） ---- */
    int              port;           // I2S 端口号（I2S_NUM_0 / I2S_NUM_1）
    BSP_I2S_Dir_t    dir;            // 方向：收 / 发
    uint32_t         sample_rate;    // 采样率 Hz
    int              pin_bclk;       // 位时钟（SCK / BCLK）
    int              pin_ws;         // 帧时钟（WS / LRC）
    int              pin_data;       // 数据（SD / DIN）
    const char      *name;           // 日志名

    /* ---- 运行时状态 ---- */
    i2s_chan_handle_t       chan;           // 通道句柄
    i2s_chan_config_t       chan_cfg;       // 通道配置（Start 时复用）
    i2s_std_config_t        std_cfg;        // 标准模式配置
    uint8_t                 gain_shift;     // RX 数字增益右移位数
    uint8_t                 volume;         // TX 软件音量 0~100
    uint8_t                 started;        // 通道是否已使能
    uint8_t                 inited;         // 是否已 init
};

/**
 * @brief 硬件映射表：端口、引脚、方向、采样率
 * @note  ★ 引脚全部落在 ESP32-S3-N16R8 安全区，禁区说明见 bsp_gpio.c 顶部 ★
 *        ★ 表中每个宏的用意：I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG 指定标准 I2S 时序
 *          （BCLK 上升沿采样、WS 左对齐），INMP441 与 MAX98357 都兼容这个模式 ★
 */
static const struct BSP_I2S_Handle I2S_HW_MAP[] = {
    /* [0] INMP441 麦克风：只收，I2S_NUM_0 */
    {
        .port        = I2S_NUM_0,
        .dir         = BSP_I2S_DIR_RX,
        .sample_rate = BSP_I2S_SAMPLE_RATE,
        .pin_bclk    = 41,      /* INMP441 SCK */
        .pin_ws      = 42,      /* INMP441 WS  */
        .pin_data    = 47,      /* INMP441 SD  */
        .name        = "MIC(INMP441)",
    },
    /* [1] MAX98357A 扬声器：只发，I2S_NUM_1 */
    {
        .port        = I2S_NUM_1,
        .dir         = BSP_I2S_DIR_TX,
        .sample_rate = BSP_I2S_SAMPLE_RATE,
        .pin_bclk    = 3,       /* MAX98357 BCLK */
        .pin_ws      = 46,      /* MAX98357 LRC  */
        .pin_data    = 2,       /* MAX98357 DIN  */
        .name        = "SPK(MAX98357)",
    },
};

/*==============================================================================
 * 具名句柄
 *============================================================================*/
/**
 * @brief 具名句柄，指向映射表中的表项
 * @note  ★ 新增句柄时必须同时加进 I2S_HW_MAP ★
 *        ★ 运行时状态（chan / gain_shift / volume 等）要在 Start 时写进表项，
 *          所以这里必须强转掉 const —— 表项的「配置部分」逻辑上仍是只读的，
 *          只有运行时字段被改写。这是句柄表方案的既定代价：
 *          好处是上层拿到的指针类型是 const，写不了。★
 */
static struct BSP_I2S_Handle I2S_RT_MAP[ARRAY_SIZE(I2S_HW_MAP)];

const BSP_I2S_Handle_t *const g_i2s_mic = &I2S_RT_MAP[0];
const BSP_I2S_Handle_t *const g_i2s_spk = &I2S_RT_MAP[1];

/*==============================================================================
 * 内部函数
 *============================================================================*/
/**
 * @brief 把配置表的静态字段拷进运行时表
 * @note 只拷静态配置，运行时字段清零。Init 时调一次。
 */
static void BSP_I2S_LoadRuntimeMap(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(I2S_HW_MAP); i++)
    {
        memset(&I2S_RT_MAP[i], 0, sizeof(I2S_RT_MAP[i]));
        I2S_RT_MAP[i].port        = I2S_HW_MAP[i].port;
        I2S_RT_MAP[i].dir         = I2S_HW_MAP[i].dir;
        I2S_RT_MAP[i].sample_rate = I2S_HW_MAP[i].sample_rate;
        I2S_RT_MAP[i].pin_bclk    = I2S_HW_MAP[i].pin_bclk;
        I2S_RT_MAP[i].pin_ws      = I2S_HW_MAP[i].pin_ws;
        I2S_RT_MAP[i].pin_data    = I2S_HW_MAP[i].pin_data;
        I2S_RT_MAP[i].name        = I2S_HW_MAP[i].name;

        I2S_RT_MAP[i].chan        = NULL;
        I2S_RT_MAP[i].gain_shift  = BSP_I2S_DEFAULT_GAIN_SHIFT;
        I2S_RT_MAP[i].volume      = 100;
        I2S_RT_MAP[i].started     = 0;
        I2S_RT_MAP[i].inited      = 0;
    }
}

/**
 * @brief 建立并启动一个 I2S 句柄的通道
 * @param h 运行时句柄指针（可写）
 * @retval 0: 成功
 * @retval -1: 失败
 *
 * @note 标准驱动（driver/i2s_std.h）的四步流程：
 *         1. i2s_new_channel()        —— 申请通道，指定端口和方向
 *         2. i2s_channel_init_std_mode() —— 下发时钟/槽位/引脚配置
 *         3. i2s_channel_enable()     —— 使能，开始出时钟
 *         4. 读/写用 i2s_channel_read/write
 */
static int BSP_I2S_StartHandle(struct BSP_I2S_Handle *h)
{
    if (h == NULL)
        return -1;

    if (h->started)
        return 0;   /* 已启动，幂等 */

    /* ---------- 1. 申请通道 ---------- */
    if (h->chan == NULL)
    {
        h->chan_cfg = (i2s_chan_config_t){
            .id             = (i2s_port_t)h->port,
            .role           = I2S_ROLE_MASTER,   /* 两种方向都是主机：BCLK/WS 由 ESP32 产生 */
            .dma_desc_num   = BSP_I2S_DMA_DESC_NUM,
            .dma_frame_num  = BSP_I2S_DMA_FRAME_NUM,
            /* ★ auto_clear：仅 TX 需要。写完后 DMA 缓冲自动补零，
             *   否则空闲时会把上一段音频的残留数据反复发出去（表现为扬声器嗡嗡声）。
             *   注意该字段是 union 的别名，与 auto_clear_before_cb 互斥。 */
            .auto_clear_after_cb = (h->dir == BSP_I2S_DIR_TX) ? true : false,
            .allow_pd       = false,
            .intr_priority  = 0,
        };

        /* 单工：只填对应方向的那个出参，另一个传 NULL */
        i2s_chan_handle_t *tx_out = (h->dir == BSP_I2S_DIR_TX) ? &h->chan : NULL;
        i2s_chan_handle_t *rx_out = (h->dir == BSP_I2S_DIR_RX) ? &h->chan : NULL;

        esp_err_t err = i2s_new_channel(&h->chan_cfg, tx_out, rx_out);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "%s: i2s_new_channel failed: %s", h->name, esp_err_to_name(err));
            h->chan = NULL;
            return -1;
        }
    }

    /* ---------- 2. 配置标准模式（时钟 + 槽位 + 引脚） ---------- */
    /* 所有通道都是 I2S 主机：BCLK 和 WS 由 ESP32 产生。
     * ★ INMP441 与 MAX98357 都是纯从机，必须由 ESP32 主出时钟 ★ */
    h->std_cfg = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(h->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,            /* 不用 MCLK，模块内部有 PLL */
            .bclk = (gpio_num_t)h->pin_bclk,
            .ws   = (gpio_num_t)h->pin_ws,
            .dout = (h->dir == BSP_I2S_DIR_TX) ? (gpio_num_t)h->pin_data : I2S_GPIO_UNUSED,
            .din  = (h->dir == BSP_I2S_DIR_RX) ? (gpio_num_t)h->pin_data : I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /* ★★ 关键：单声道 + 左声道的组合 ★★
     * I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG 默认 slot_mask = I2S_STD_SLOT_BOTH，
     * 在 MONO 模式下如果不动它，DMA 里会有「一个有效样点 + 一个空样点」交替出现，
     * 表现为音量只有一半、且听到 8kHz 的嗡嗡声。
     * INMP441 的 L/R 脚接地 = 数据在左声道输出 → 必须显式指定 LEFT。 */
    h->std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    esp_err_t err = i2s_channel_init_std_mode(h->chan, &h->std_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s: init_std_mode failed: %s", h->name, esp_err_to_name(err));
        i2s_del_channel(h->chan);
        h->chan = NULL;
        return -1;
    }

    /* ---------- 3. 使能 ---------- */
    err = i2s_channel_enable(h->chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s: channel_enable failed: %s", h->name, esp_err_to_name(err));
        i2s_del_channel(h->chan);
        h->chan = NULL;
        return -1;
    }

    h->started = 1;
    h->inited  = 1;

    ESP_LOGI(TAG, "%s started: port=%d, %u Hz, mono, %d bit data in 32 bit slot, "
                  "bclk=IO%d ws=IO%d data=IO%d",
             h->name, h->port, (unsigned)h->sample_rate, BSP_I2S_BITS_PER_SAMPLE,
             h->pin_bclk, h->pin_ws, h->pin_data);
    return 0;
}

/**
 * @brief 帧数转毫秒（向上取整）
 */
static uint32_t BSP_I2S_FramesToMs(uint32_t frames, uint32_t sample_rate)
{
    return (uint32_t)(((uint64_t)frames * 1000ULL + sample_rate - 1) / sample_rate);
}

/*==============================================================================
 * 初始化 / 反初始化
 *============================================================================*/
/**
 * @brief I2S BSP 层初始化（两个端口全部建立通道并使能）
 */
int BSP_I2S_Init(void)
{
    BSP_I2S_LoadRuntimeMap();

    int ret = 0;
    for (size_t i = 0; i < ARRAY_SIZE(I2S_RT_MAP); i++)
    {
        if (BSP_I2S_StartHandle(&I2S_RT_MAP[i]) != 0)
            ret = -1;
    }
    return ret;
}

/**
 * @brief 建立并启动单个 I2S 句柄对应的通道
 */
int BSP_I2S_Start(const BSP_I2S_Handle_t *handle)
{
    if (handle == NULL)
    {
        int ret = 0;
        for (size_t i = 0; i < ARRAY_SIZE(I2S_RT_MAP); i++)
        {
            if (BSP_I2S_StartHandle(&I2S_RT_MAP[i]) != 0)
                ret = -1;
        }
        return ret;
    }

    /* 句柄是 const 指针，但运行时字段要写；表项本来就是非 const 的 */
    return BSP_I2S_StartHandle((struct BSP_I2S_Handle *)handle);
}

/**
 * @brief 停止通道（保留配置，可再 Start）
 */
int BSP_I2S_Stop(const BSP_I2S_Handle_t *handle)
{
    if (handle == NULL)
    {
        int ret = 0;
        for (size_t i = 0; i < ARRAY_SIZE(I2S_RT_MAP); i++)
        {
            if (BSP_I2S_Stop(&I2S_RT_MAP[i]) != 0)
                ret = -1;
        }
        return ret;
    }

    struct BSP_I2S_Handle *h = (struct BSP_I2S_Handle *)handle;
    if (h->chan == NULL)
        return 0;

    esp_err_t err = i2s_channel_disable(h->chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s: channel_disable failed: %s", h->name, esp_err_to_name(err));
        return -1;
    }

    h->started = 0;
    return 0;
}

/*==============================================================================
 * 采集（麦克风）
 *============================================================================*/
/**
 * @brief 从麦克风读取音频（32bit 槽 → 16bit PCM，带数字增益）
 */
int BSP_I2S_Read(const BSP_I2S_Handle_t *handle, int16_t *buf, size_t frames, uint32_t timeout_ms)
{
    return BSP_I2S_ReadWithStats(handle, buf, frames, timeout_ms, NULL);
}

/**
 * @brief 从麦克风读取音频，顺便算好诊断统计量
 * @note  i2s_channel_read 一次不一定读满，所以这里循环累积，直到读够或超时。
 *        音频流是连续的，漏读会让时间轴错位，所以必须读满调用方要的量。
 */
int BSP_I2S_ReadWithStats(const BSP_I2S_Handle_t *handle, int16_t *buf, size_t frames,
                          uint32_t timeout_ms, BSP_I2S_Stats_t *stats)
{
    if (handle == NULL || buf == NULL || frames == 0)
        return -1;

    struct BSP_I2S_Handle *h = (struct BSP_I2S_Handle *)handle;
    if (h->dir != BSP_I2S_DIR_RX || h->chan == NULL || !h->started)
        return -1;

    /* 32bit 槽：1 帧 = 4 字节，用临时缓冲接 DMA 数据 */
    static int32_t raw[512];
    const size_t   raw_cap = ARRAY_SIZE(raw);

    size_t   got        = 0;
    uint32_t t_start    = BSP_SYS_GetTickMs();
    uint32_t elapsed_ms = 0;

    while (got < frames)
    {
        size_t want = frames - got;
        if (want > raw_cap)
            want = raw_cap;

        /* 剩余超时时间：至少给 1ms，避免 0 超时被当成「不等待」 */
        uint32_t remain_ms = 1;
        if (timeout_ms > 0)
        {
            elapsed_ms = BSP_SYS_GetTickMs() - t_start;
            if (elapsed_ms >= timeout_ms)
                break;
            remain_ms = timeout_ms - elapsed_ms;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(h->chan,
                                         raw,
                                         want * sizeof(int32_t),
                                         &bytes_read,
                                         pdMS_TO_TICKS(remain_ms));

        /* ★ 先搬移已读到的数据，再判错误（审计修复）★
         *
         * 原来是在判错之后才搬移，于是 ESP_ERR_TIMEOUT 时即使 bytes_read > 0
         * 也会走 continue/break，把已经读进 raw[] 的样点直接丢掉。
         * 而 ESP-IDF 的 i2s_channel_read **确实可能**在超时时返回部分数据
         * （已核对 IDF 5.4.4 的 i2s_common.c：它先把 DMA 里已有的数据搬给调用方，
         *  只有当一帧都没拿到时才单纯返回 ESP_ERR_TIMEOUT）。
         *
         * 丢样点的后果：录音里出现短促的静音缝隙，时间轴整体前移 ——
         * 听感上是"咔"一下，而 Whisper 的识别率也会下降。
         *
         * 所以正确顺序是：先把 bytes_read 的内容转换进输出缓冲，再看错误码决定
         * 是继续等（阻塞读）还是收工（非阻塞读）。 */
        size_t n = bytes_read / sizeof(int32_t);

        if (n > 0)
        {
            /* ★ INMP441 是 24bit 左对齐塞在 32bit 槽里：低 8 位是无效位，
             *   取高 16 位（>> 16）得到原始电平；默认再右移 2 位换取 4 倍增益。
             *   理由见 bsp_i2s.h。 */
            for (size_t i = 0; i < n; i++)
            {
                int32_t v = raw[i] >> h->gain_shift;
                buf[got + i] = (int16_t)CLAMP_I16(v);
            }
            got += n;
        }

        if (err == ESP_ERR_TIMEOUT)
        {
            /* 数据已经收下了，这里只需要决定还要不要继续等 */
            if (timeout_ms == 0 || n == 0)
            {
                if (timeout_ms == 0)
                    break;          /* 非阻塞读：读不到就返回 */
                continue;           /* 阻塞读但一个样点都没拿到：继续等 */
            }
            continue;               /* 拿到了部分数据，继续凑满 */
        }

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "%s: read failed: %s", h->name, esp_err_to_name(err));
            break;
        }

        if (n == 0)
        {
            if (timeout_ms == 0)
                break;
            continue;
        }

    }

    /* ---------- 统计量 ---------- */
    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
        stats->frames = (uint32_t)got;

        if (got > 0)
        {
            double sum = 0.0, sum_sq = 0.0;
            int32_t peak_abs = 0;
            uint8_t clipped = 0;

            for (size_t i = 0; i < got; i++)
            {
                int32_t v = buf[i];
                int32_t a = (v < 0) ? -v : v;

                sum    += (double)v;
                sum_sq += (double)v * (double)v;
                if (a > peak_abs)
                    peak_abs = a;
                if (a >= 32767)
                    clipped = 1;
            }

            stats->rms     = (float)(sqrt(sum_sq / (double)got) / 32768.0);
            stats->peak    = (float)((double)peak_abs / 32768.0);
            stats->dc      = (float)((sum / (double)got) / 32768.0);
            stats->clipped = clipped;
        }
    }

    return (int)got;
}

/*==============================================================================
 * 播放（扬声器）
 *============================================================================*/
/**
 * @brief 向扬声器写入音频
 * @note  16bit → 32bit 槽：左移 16 位，低 16 位补 0。
 *        MAX98357 取槽内高位，所以这样放不会损失精度。
 */
int BSP_I2S_Write(const BSP_I2S_Handle_t *handle, const int16_t *buf, size_t frames, uint32_t timeout_ms)
{
    if (handle == NULL || buf == NULL || frames == 0)
        return -1;

    struct BSP_I2S_Handle *h = (struct BSP_I2S_Handle *)handle;
    if (h->dir != BSP_I2S_DIR_TX || h->chan == NULL || !h->started)
        return -1;

    static int32_t raw[512];
    const size_t   raw_cap = ARRAY_SIZE(raw);

    size_t   sent = 0;
    uint32_t t_start = BSP_SYS_GetTickMs();

    while (sent < frames)
    {
        size_t chunk = frames - sent;
        if (chunk > raw_cap)
            chunk = raw_cap;

        /* 软件音量 + 16bit → 32bit 槽 */
        for (size_t i = 0; i < chunk; i++)
        {
            int32_t v = buf[sent + i];
            if (h->volume < 100)
                v = (v * (int32_t)h->volume) / 100;
            raw[i] = v << 16;
        }

        /* 超时预算：按剩余帧数估算播放时间，再叠加调用方给的余量 */
        uint32_t wait_ms = BSP_I2S_FramesToMs((uint32_t)chunk, h->sample_rate) + 100;
        if (timeout_ms > 0)
        {
            uint32_t elapsed = BSP_SYS_GetTickMs() - t_start;
            if (elapsed >= timeout_ms)
                break;
            uint32_t remain = timeout_ms - elapsed;
            if (wait_ms > remain)
                wait_ms = remain;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(h->chan,
                                          raw,
                                          chunk * sizeof(int32_t),
                                          &bytes_written,
                                          pdMS_TO_TICKS(wait_ms));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "%s: write failed: %s", h->name, esp_err_to_name(err));
            break;
        }

        size_t n = bytes_written / sizeof(int32_t);
        if (n == 0)
            break;

        sent += n;
    }

    return (int)sent;
}

/**
 * @brief 播放一段静音
 */
int BSP_I2S_WriteSilence(const BSP_I2S_Handle_t *handle, size_t frames)
{
    if (handle == NULL || frames == 0)
        return -1;

    static const int16_t zero[256] = {0};
    size_t remain = frames;

    while (remain > 0)
    {
        size_t chunk = (remain > ARRAY_SIZE(zero)) ? ARRAY_SIZE(zero) : remain;
        int n = BSP_I2S_Write(handle, zero, chunk, 1000);
        if (n <= 0)
            return (int)(frames - remain);
        remain -= (size_t)n;
    }
    return (int)frames;
}

/*==============================================================================
 * 音量与增益
 *============================================================================*/
/**
 * @brief 设置录音数字增益的右移位数
 */
int BSP_I2S_SetGainShift(const BSP_I2S_Handle_t *handle, uint8_t gain_shift)
{
    if (handle == NULL || gain_shift < 8 || gain_shift > 20)
        return -1;

    struct BSP_I2S_Handle *h = (struct BSP_I2S_Handle *)handle;
    h->gain_shift = gain_shift;
    ESP_LOGI(TAG, "%s: gain_shift = %u (>> %u)", h->name, gain_shift, gain_shift);
    return 0;
}

/**
 * @brief 设置播放软件音量
 */
int BSP_I2S_SetVolume(const BSP_I2S_Handle_t *handle, uint8_t volume)
{
    if (handle == NULL || volume > 100)
        return -1;

    struct BSP_I2S_Handle *h = (struct BSP_I2S_Handle *)handle;
    h->volume = volume;
    ESP_LOGI(TAG, "%s: volume = %u%%", h->name, volume);
    return 0;
}

/*==============================================================================
 * WAV 封装
 *============================================================================*/
/**
 * @brief 小端写 16bit / 32bit
 */
static void wav_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/**
 * @brief 生成标准 44 字节 WAV 头（RIFF/WAVE，PCM，16bit，单声道）
 * @note  字段布局（每行前半是偏移）：
 *          0  "RIFF" | 4  chunkSize=36+data | 8  "WAVE"
 *          12 "fmt " | 16 subchunk1Size=16   | 20 audioFormat=1(PCM)
 *          22 channels=1 | 24 sampleRate     | 28 byteRate
 *          32 blockAlign | 34 bitsPerSample=16
 *          36 "data" | 40 dataSize
 *        后端会校验 audioFormat=1 和 bitsPerSample=16，改错这两处会被 400 拒掉。
 */
int BSP_I2S_BuildWavHeader(uint8_t *header, uint32_t data_bytes, uint32_t sample_rate)
{
    if (header == NULL || sample_rate == 0)
        return -1;

    const uint16_t channels      = BSP_I2S_CHANNELS;
    const uint16_t bits          = BSP_I2S_BITS_PER_SAMPLE;
    const uint16_t block_align   = (uint16_t)(channels * bits / 8);
    const uint32_t byte_rate     = sample_rate * block_align;

    /* "RIFF" */
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    wav_put_u32(header + 4, 36 + data_bytes);
    /* "WAVE" */
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    /* "fmt " */
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    wav_put_u32(header + 16, 16);
    wav_put_u16(header + 20, 1);            /* audioFormat = 1 → PCM */
    wav_put_u16(header + 22, channels);
    wav_put_u32(header + 24, sample_rate);
    wav_put_u32(header + 28, byte_rate);
    wav_put_u16(header + 32, block_align);
    wav_put_u16(header + 34, bits);
    /* "data" */
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    wav_put_u32(header + 40, data_bytes);

    return 0;
}
