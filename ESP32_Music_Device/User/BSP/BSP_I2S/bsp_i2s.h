/**
 * @file bsp_i2s.h
 * @brief I2S 音频 BSP 层（句柄式接口，麦克风采集 + 扬声器播放）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 上层用 BSP 暴露的具名句柄操作音频口，不传端口号、不传引脚号：
 *
 *           BSP_I2S_Read(g_i2s_mic, buf, frames);
 *           BSP_I2S_Write(g_i2s_spk, buf, frames);
 *
 *       ┌──────────────┬──────────┬──────────────────────────────────────────┐
 *       │ 句柄          │ I2S 端口 │ 外设                                     │
 *       ├──────────────┼──────────┼──────────────────────────────────────────┤
 *       │ g_i2s_mic    │ I2S_NUM_0│ INMP441（只收，I2S 主机，PHILIPS 标准）  │
 *       │ g_i2s_spk    │ I2S_NUM_1│ MAX98357A（只发，I2S 主机，PHILIPS 标准）│
 *       └──────────────┴──────────┴──────────────────────────────────────────┘
 *
 *       ★ 为什么用两个独立端口而不是一个全双工口？
 *         INMP441 与 MAX98357 对 BCLK/WS 的时序容限不同（MAX98357 对 BCLK 占空比敏感，
 *         某些模块在 slave 模式下要求严格 50%），两个独立主机口各自跑各自的时钟最稳；
 *         而且 ESP32-S3 有 2 个 I2S 控制器，分开后互不干扰，也方便单独关掉一路省电。
 *
 *       ★ 格式约定（与后端 API 契约一致，见 esp32-music/docs/API.md §2）：
 *         采样率固定 16000Hz、单声道、16bit PCM —— 这是 Whisper 的原生格式，
 *         固件端不做重采样，省 CPU 也省一次精度损失。
 *
 *       ★ 内部一律用 32bit 槽传输（I2S_DATA_BIT_WIDTH_32BIT）：
 *         - INMP441 物理输出就是 24bit 左对齐塞在 32bit 槽里，按 32bit 读再取高 16 位；
 *         - MAX98357 接收 32bit 槽时取高位，16bit 数据左移 16 位放进去即可。
 *         若把槽位配成 16bit，INMP441 会丢掉有效位、MAX98357 会出现量化噪声。
 *
 *       句柄结构体对上层不透明，因此本头文件不依赖 ESP-IDF。
 */

#ifndef __BSP_I2S_H__
#define __BSP_I2S_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
#define BSP_I2S_MAX_NUM         2       /* I2S 实例最大数量（ESP32-S3 有 2 个控制器） */

#define BSP_I2S_SAMPLE_RATE     16000   /* 采样率：Whisper 原生，接口文档规定值 */
#define BSP_I2S_CHANNELS        1       /* 单声道 */
#define BSP_I2S_BITS_PER_SAMPLE 16      /* 有效位深 16bit */

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief I2S 句柄（不透明类型，成员定义在 bsp_i2s.c）
 */
typedef struct BSP_I2S_Handle BSP_I2S_Handle_t;

/**
 * @brief I2S 方向
 */
typedef enum {
    BSP_I2S_DIR_RX = 0,     // 只收（麦克风）
    BSP_I2S_DIR_TX = 1,     // 只发（扬声器）
} BSP_I2S_Dir_t;

/**
 * @brief 一次采集的音频诊断信息
 * @note  用来判断「麦克风到底有没有出声」——硬件到货后先看这组数：
 *        rms 约 0.01~0.15 属正常（说话时），恒为 0 说明数据线/时钟线有问题。
 */
typedef struct {
    uint32_t frames;        // 本次采集的样点数
    float    rms;           // 均方根（0..1，1.0 对应满量程）
    float    peak;          // 峰值（0..1）
    float    dc;            // 直流偏置（-1..1，理想值 0）
    uint8_t  clipped;       // 是否有样点削顶（1=有）
} BSP_I2S_Stats_t;

/*==============================================================================
 * 具名句柄
 *============================================================================*/
/**
 * @brief 具名句柄：端口、引脚、采样率全部写死在 bsp_i2s.c 的映射表里
 */
extern const BSP_I2S_Handle_t *const g_i2s_mic;     /* INMP441  采集（I2S_NUM_0） */
extern const BSP_I2S_Handle_t *const g_i2s_spk;     /* MAX98357 播放（I2S_NUM_1） */

/*==============================================================================
 * 初始化 / 反初始化
 *============================================================================*/
/**
 * @brief I2S BSP 层初始化（两个端口全部建立通道并使能）
 * @retval 0: 全部成功
 * @retval -1: 有端口初始化失败（串口里能看到具体是哪个）
 * @note  未接外设时也能正常返回：I2S 主机即使没有从机也会照常输出时钟，
 *        所以「麦克风还没到货」不影响本函数成功。
 */
int BSP_I2S_Init(void);

/**
 * @brief 建立并启动单个 I2S 句柄对应的通道
 * @param handle 句柄（NULL 则操作全部句柄）
 * @retval 0: 成功
 * @retval -1: 失败
 * @note  BSP_I2S_Init() 内部会调用，一般不用手动调。
 */
int BSP_I2S_Start(const BSP_I2S_Handle_t *handle);

/**
 * @brief 停止通道（保留配置，可再 Start）
 * @param handle 句柄（NULL 则操作全部句柄）
 * @retval 0: 成功
 * @retval -1: 失败
 */
int BSP_I2S_Stop(const BSP_I2S_Handle_t *handle);

/*==============================================================================
 * 采集（麦克风）
 *============================================================================*/
/**
 * @brief 从麦克风读取音频（32bit 槽 → 16bit PCM，带数字增益）
 * @param handle 句柄（应为 g_i2s_mic）
 * @param buf    输出缓冲区（int16_t，主机字节序，小端）
 * @param frames 期望读取的样点数（单声道，1 样点 = 2 字节）
 * @param timeout_ms 超时（毫秒），0 = 不等待（读多少算多少）
 * @retval >0: 实际读到的样点数
 * @retval  0: 超时，一个都没读到
 * @retval -1: 参数错误或该句柄不是 RX 方向
 * @note  转换公式：sample = clamp(raw >> gain_shift)，gain_shift 默认 **16**（不额外增益）。
 *        INMP441 输出 24bit 有效数据左对齐在 32bit 槽里，理论上是 >> 16 取高 16 位；
 *        但实测 INMP441 满量程很少跑满，>> 16 得到的 RMS 往往偏低（< 0.01），
 *        ★ 实测（2026-10）证明 14 会削顶，所以默认已改成 16（见 bsp_i2s.c 的详细说明）★
 *        若换模块后发现 rms 偏小（< 0.01），再调回 15（2 倍）或 14（4 倍）。
 */
int BSP_I2S_Read(const BSP_I2S_Handle_t *handle, int16_t *buf, size_t frames, uint32_t timeout_ms);

/**
 * @brief 从麦克风读取音频，顺便算好诊断统计量
 * @param handle 句柄
 * @param buf    输出缓冲区
 * @param frames 期望读取的样点数
 * @param timeout_ms 超时（毫秒）
 * @param stats  输出统计量（可为 NULL）
 * @retval 同 BSP_I2S_Read
 */
int BSP_I2S_ReadWithStats(const BSP_I2S_Handle_t *handle, int16_t *buf, size_t frames,
                          uint32_t timeout_ms, BSP_I2S_Stats_t *stats);

/*==============================================================================
 * 播放（扬声器）
 *============================================================================*/
/**
 * @brief 向扬声器写入音频
 * @param handle 句柄（应为 g_i2s_spk）
 * @param buf    输入缓冲区（int16_t PCM）
 * @param frames 要写入的样点数
 * @param timeout_ms 超时（毫秒）
 * @retval >0: 实际写入的样点数
 * @retval  0: 超时
 * @retval -1: 参数错误或该句柄不是 TX 方向
 * @note  内部把 16bit 左移 16 位塞进 32bit 槽。
 *        MAX98357 的 BCLK/LRC 由 ESP32 主出，SD 数据在 BCLK 下降沿被采样。
 */
int BSP_I2S_Write(const BSP_I2S_Handle_t *handle, const int16_t *buf, size_t frames, uint32_t timeout_ms);

/**
 * @brief 播放一段静音（用于上电消音、或给 MAX98357 提供时钟让它完成内部上电时序）
 * @param handle 句柄
 * @param frames 静音样点数（16000 样点 = 1 秒）
 * @retval 同 BSP_I2S_Write
 * @note  MAX98357 上电后需要先收到一段时钟才会使能输出，否则首个音符会被吃掉；
 *        建议开机时先 WriteSilence() 约 100ms。
 */
int BSP_I2S_WriteSilence(const BSP_I2S_Handle_t *handle, size_t frames);

/*==============================================================================
 * 音量与增益
 *============================================================================*/
/**
 * @brief 设置录音数字增益的右移位数
 * @param handle     句柄
 * @param gain_shift 右移位数：16 = 原始电平（默认），14 = 4 倍增益，12 = 16 倍增益
 * @retval 0: 成功
 * @retval -1: 参数错误（shift 必须在 8~20 之间）
 * @note  只对 RX 方向有效。默认值见 BSP_I2S_DEFAULT_GAIN_SHIFT。
 */
int BSP_I2S_SetGainShift(const BSP_I2S_Handle_t *handle, uint8_t gain_shift);

/**
 * @brief 设置播放软件音量
 * @param handle 句柄
 * @param volume 0~100（0 = 静音，100 = 原始电平）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  直接乘在 PCM 数据上，属于软件音量；MAX98357 的硬件增益由 GAIN 引脚的分压决定。
 *        想在硬件上调音量，改 GAIN 脚与 GND 之间的电阻即可，不用改代码。
 */
int BSP_I2S_SetVolume(const BSP_I2S_Handle_t *handle, uint8_t volume);

/*==============================================================================
 * WAV 封装（供后端上传用）
 *============================================================================*/
#define BSP_I2S_WAV_HEADER_SIZE 44      /* 标准 RIFF/WAVE 头长度 */

/**
 * @brief 生成标准 44 字节 WAV 头（RIFF/WAVE，PCM，16bit，单声道）
 * @param header     输出缓冲区，至少 44 字节
 * @param data_bytes 后续 PCM 数据的字节数
 * @param sample_rate 采样率（用 BSP_I2S_SAMPLE_RATE）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  后端要求严格的标准 WAV（audioFormat = 1），不接受浮点或压缩格式。
 *        本函数手工按字节小端写入，不依赖任何 WAV 库，也顺手把
 *        「RIFF/WAVE/fmt /data」四个魔数写死，避免字节序踩坑。
 */
int BSP_I2S_BuildWavHeader(uint8_t *header, uint32_t data_bytes, uint32_t sample_rate);

#endif /* __BSP_I2S_H__ */
