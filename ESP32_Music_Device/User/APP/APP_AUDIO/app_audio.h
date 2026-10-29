/**
 * @file app_audio.h
 * @brief 音频应用层（录音缓冲 + 统一播放任务）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本模块解决两个问题：
 *
 *   1) 录音：把麦克风的 PCM 流按固定长度收进一块缓冲，并记录实际时长
 *   2) 播放：所有发声都走同一个播放任务，音源可插拔
 *
 *==============================================================================
 * ★ 为什么播放要统一成一个任务 + 可插拔音源 ★
 *==============================================================================
 *  本工程有三类要发声的东西：
 *      a) 麦克风录下来的原始 PCM（回环测试回放）
 *      b) 后端下发参数经 KS 实时合成出来的音频
 *      c) 未来可能的内存 WAV 文件
 *
 *  它们的共同点是「都要按 16kHz 实时往 I2S 灌样点」，区别只在「样点从哪来」。
 *  如果给每种各写一个播放循环，就会出现三份几乎一样的代码，
 *  而且它们会互相抢 I2S —— 两个任务同时 i2s_channel_write 会导致
 *  DMA 数据交错、声音完全乱掉。
 *
 *  所以这里定义一个「音源」接口（App_AudioSource_t），
 *  播放任务只认接口，不认具体是什么：
 *      - 缓冲音源：从内存线性读，支持循环（回环回放 / 未来 WAV）
 *      - 回调音源：调用方自己填缓冲（KS 合成器就挂在这个上）
 *  以后加新音源只需要写一个 fill 回调，播放任务一行都不用改。
 *
 *  这个设计还有个副作用的好处：KS 合成不需要预先渲染整首曲子到内存
 *  （42 秒 × 16kHz × 2 字节 = 1.3MB），而是边播边算，PSRAM 占用为 0。
 */

#ifndef __APP_AUDIO_H__
#define __APP_AUDIO_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 音源：给播放任务用的「样点生产者」接口
 */
typedef struct App_AudioSource {
    /**
     * @brief 填充 PCM
     * @param self   音源自身
     * @param out    输出缓冲（int16 单声道）
     * @param frames 期望样点数
     * @retval >0: 实际填入的样点数
     * @retval  0: 本轮没有数据（播放任务会稍等再试，适合流式音源）
     * @retval <0: 音源已结束（播放任务据此收尾）
     */
    int (*fill)(struct App_AudioSource *self, int16_t *out, size_t frames);

    /**
     * @brief 重置到开头
     * @param self 音源自身
     * @note  循环播放每轮开始、以及「再次播放从头开始」时调用。
     */
    void (*rewind)(struct App_AudioSource *self);

    void *user;         // 音源私有数据
} App_AudioSource_t;

/**
 * @brief 线性缓冲音源（回环回放、内存 WAV 播放用）
 */
typedef struct {
    App_AudioSource_t base;     // ★ 必须放第一个成员，便于安全向上/向下转换 ★
    const int16_t    *pcm;      // 数据
    size_t            frames;   // 总样点数
    size_t            pos;      // 当前播放位置
} App_AudioLinearSrc_t;

/*==============================================================================
 * 初始化
 *============================================================================*/
/**
 * @brief 音频应用层初始化
 * @retval 0: 成功
 * @retval -1: 失败（I2S 建立失败或 PSRAM 分配失败）
 * @note  做三件事：
 *          1. BSP_I2S_Init() 建立麦克风（I2S0）和扬声器（I2S1）通道
 *          2. 给扬声器送一段静音预热（MAX98357 需要先收到时钟才使能输出）
 *          3. 在 PSRAM 里分配录音缓冲和回环缓冲
 *        ★ 麦克风/扬声器没到货时本函数依然成功 ★
 *          I2S 主机即使没有从机也会照常输出时钟，不会报错。
 *          所以「硬件没到」不影响初始化，只是录不到/听不到。
 */
int App_Audio_Init(void);

/*==============================================================================
 * 周期服务（在音频任务里调用）
 *============================================================================*/
/**
 * @brief 音频命令处理 + 播放推进
 * @param timeout_ms 等待命令的超时（毫秒）
 * @note  ★ 播放任务的主体就是这个函数 ★
 *        它先看有没有播放命令（阻塞等待 timeout_ms），
 *        有命令就切换音源；然后从当前音源取一块 PCM 写给扬声器。
 *        i2s_channel_write 会阻塞到 DMA 有空位，所以本函数天然按实时速度运行。
 */
void App_Audio_Service(uint32_t timeout_ms);

/*==============================================================================
 * 播放控制
 *============================================================================*/
/**
 * @brief 开始播放某个音源
 * @param src   音源（必须一直存活到播放结束）
 * @param loop  1 = 循环播放，0 = 只播一遍
 * @param stop_current 1 = 先停掉正在播的（同一时刻只允许一个音源发声）
 * @retval 0: 命令已下发
 * @retval -1: 参数错误
 * @note  ★ 非阻塞：命令通过队列交给播放任务执行 ★
 *        这样按键任务不会被 I2S 写入阻塞。
 */
int App_Audio_Play(const App_AudioSource_t *src, uint8_t loop, uint8_t stop_current);

/**
 * @brief 停止播放（清空队列里的待播命令）
 * @retval 0: 命令已下发
 */
int App_Audio_Stop(void);

/**
 * @brief 查询是否正在播放
 * @retval 1: 正在播
 * @retval 0: 空闲
 */
uint8_t App_Audio_IsPlaying(void);

/**
 * @brief 查询当前音源是否是给定指针（用于「再按一次停止」的逻辑）
 * @param src 音源
 * @retval 1: 是
 * @retval 0: 否
 */
uint8_t App_Audio_IsPlayingSource(const App_AudioSource_t *src);

/*==============================================================================
 * 线性缓冲音源
 *============================================================================*/
/**
 * @brief 初始化一个线性缓冲音源
 * @param s      音源结构体（调用方分配，需长期存活）
 * @param pcm    PCM 数据（int16 单声道，16kHz）
 * @param frames 样点数
 */
void App_Audio_LinearSrcInit(App_AudioLinearSrc_t *s, const int16_t *pcm, size_t frames);

/*==============================================================================
 * 合成器所有权接口（★ 只允许音频任务碰合成器对象 ★）
 *============================================================================*/
/**
 * @brief 请求音频任务重新初始化合成器，并阻塞等回执
 * @param synth ks_synth_t*（用 void* 传，保持 APP_AUDIO 不依赖 KS 组件）
 * @param music music_t*
 * @retval  0: 成功
 * @retval -1: 失败
 *
 * @note  ★★ 不要直接调 KS_Synth_Init() ★★
 *        KS_Synth_Init 会 memset 整个 66KB 合成器对象，而音频任务可能正在
 *        渲染同一个对象（KS_Synth_Render）→ 空指针解引用 / 除零崩溃。
 *        这是审计发现的严重缺陷，所以初始化必须由音频任务执行、调用方等回执。
 *
 *        ★ 调用方不能是音频任务自己 ★（自己等自己的回执会死锁）
 */
int App_Audio_SynthReload(void *synth, void *music);

/*==============================================================================
 * 录音
 *============================================================================*/
/**
 * @brief 录音目标缓冲
 * @note  本工程有两个录音缓冲，用途完全不同，所以做成显式目标而不是靠猜：
 *
 *          AUDIO_REC_MAIN      普通录音：KEY1 按住录、松手停 → 封装 WAV 上传后端
 *          AUDIO_REC_LOOPBACK  回环测试：KEY3 按住录、松手停 → 只留在本地，
 *                              KEY4 反复回放，用来判断麦克风到底能不能录到东西
 *
 *        ★ 为什么必须区分 ★
 *          两个缓冲容量不同、后续动作不同（一个上传一个不上传）。
 *          早期版本用"容量"隐式区分目标，那是错的，已经改掉。
 */
typedef enum {
    AUDIO_REC_MAIN = 0,
    AUDIO_REC_LOOPBACK,
} App_Audio_RecTarget_t;

/**
 * @brief 录音质量诊断（回环测试用串口判断麦克风好坏，靠的就是这几个数）
 * @note  ★ 这是没有指示灯也能判断硬件的关键 ★
 *        接好 INMP441 后对着它说话，按下再松开 KEY3，串口会打印这些值：
 *
 *          rms     约 0.01~0.15  正常（说话时）；**恒为 0 = 麦克风没数据**
 *          peak    正常应明显大于 rms；接近 1.0 说明可能削顶
 *          dc      理想为 0；明显偏离（>0.05）= L/R 脚或 slot_mask 配错
 *          clipped 1 = 有样点削顶，把 gain_shift 从 14 改回 16（当前默认已是 16）
 */
typedef struct {
    uint32_t frames;        // 录到的样点数
    uint32_t ms;            // 录音时长（毫秒）
    float    rms;           // 均方根（0..1）
    float    peak;          // 峰值（0..1）
    float    dc;            // 直流偏置（-1..1）
    uint8_t  clipped;       // 是否削顶
} App_Audio_RecStats_t;

/**
 * @brief 开始录音（把麦克风数据写进指定缓冲）
 * @param target      录到哪个缓冲（主缓冲 / 回环缓冲）
 * @param max_seconds 最长录音秒数；0 = 用该缓冲的默认上限
 * @retval 0: 命令已下发
 * @retval -1: 参数错误或缓冲不足
 * @note  ★ 非阻塞 ★ 真正的采集在 App_Audio_Service / 音频任务里做，
 *        因为麦克风和扬声器都归音频任务管，从别的任务直接读会和播放抢 I2S。
 *        ★ 录音是"按住式"：调用方在按下时 Start、松开时 Stop ★
 *        安全兜底：录满 max_seconds 会自动停止（避免按住不放把缓冲写满）。
 */
int App_Audio_RecordStart(App_Audio_RecTarget_t target, uint32_t max_seconds);

/**
 * @brief 停止录音
 * @param target 停哪个缓冲的录音（0xFFFFFFFF 风格的"任意"不需要，显式传就行）
 * @retval 已录到的样点数
 * @note  如果当前没有在录音，返回该缓冲已有的样点数（幂等，不会报错）。
 */
int App_Audio_RecordStop(App_Audio_RecTarget_t target);

/**
 * @brief 查询指定缓冲是否正在录音
 */
uint8_t App_Audio_IsRecordingTarget(App_Audio_RecTarget_t target);

/**
 * @brief 查询任意缓冲是否正在录音
 */
uint8_t App_Audio_IsRecording(void);

/**
 * @brief 查询录音是否已自动录满
 */
uint8_t App_Audio_RecordIsFull(void);

/**
 * @brief 查询指定缓冲已录到的样点数
 */
size_t App_Audio_GetRecordFrames(App_Audio_RecTarget_t target);

/**
 * @brief 取指定缓冲的 PCM 数据指针
 */
const int16_t *App_Audio_GetRecordPcm(App_Audio_RecTarget_t target);

/**
 * @brief 计算指定缓冲录音的质量诊断
 * @param target 目标缓冲
 * @param st     输出诊断
 * @retval 0: 成功（哪怕录到 0 帧也会成功，只是 rms=0）
 * @retval -1: 参数错误或还没录音
 * @note  ★ 返回 0 帧不是错误 ★
 *        0 帧恰恰是最有价值的诊断结果：说明麦克风一个样点都没给，
 *        问题在接线/供电，不在代码。
 */
int App_Audio_GetRecStats(App_Audio_RecTarget_t target, App_Audio_RecStats_t *st);

/**
 * @brief 把指定缓冲的录音封装成标准 WAV（44 字节头 + PCM）
 * @param target   目标缓冲
 * @param out      输出缓冲（至少 44 + 录音字节数）
 * @param out_cap  输出缓冲容量
 * @retval >0: WAV 总字节数
 * @retval  0: 该缓冲没有录音数据
 * @retval -1: 输出缓冲不够
 * @note  ★ 头由 BSP_I2S_BuildWavHeader 生成，格式对齐后端 API 契约：
 *          16000Hz / 单声道 / 16bit / PCM ★
 *        只有主缓冲需要转 WAV（上传用）；回环缓冲用串口诊断就够，不必转。
 */
int App_Audio_BuildWav(App_Audio_RecTarget_t target, uint8_t *out, size_t out_cap);

/**
 * @brief 取指定缓冲的录音音源（供播放用）
 * @param target 目标缓冲
 * @retval 非 NULL: 音源指针（可直接传给 App_Audio_Play）
 * @retval NULL: 该缓冲还没有录音数据
 * @note  ★ 音源是"可重置"的 ★
 *        App_Audio_Play 内部会先调 rewind，所以"停止后再按从头开始"是自动的。
 */
const App_AudioSource_t *App_Audio_GetRecSrc(App_Audio_RecTarget_t target);

/*==============================================================================
 * 回环测试便捷封装（KEY3 / KEY4 直接用，内部走上面的通用接口）
 *============================================================================*/
/**
 * @brief 回环录音：开始（KEY3 按下时调）
 * @param max_seconds 最长秒数；0 = AUDIO_LOOPBACK_SECONDS
 * @retval 0: 已下发
 * @retval -1: 失败
 */
int App_Audio_LoopbackRecordStart(uint32_t max_seconds);

/**
 * @brief 回环录音：停止（KEY3 松开时调）
 * @retval 已录到的样点数
 */
int App_Audio_LoopbackRecordStop(void);

/**
 * @brief 回环缓冲里是否已有录音
 */
uint8_t App_Audio_HasLoopback(void);

/**
 * @brief 取回环录音的音源（KEY4 播放用）
 */
const App_AudioSource_t *App_Audio_GetLoopbackSrc(void);

/**
 * @brief 取回环录音的样点数
 */
size_t App_Audio_GetLoopbackFrames(void);

/*==============================================================================
 * 状态诊断
 *============================================================================*/
/**
 * @brief 打印一次音频状态摘要（供上电自检和按键时使用）
 */
void App_Audio_DumpStatus(void);

/**
 * @brief 检查函数返回值的通用日志（内部用，公开是为了让调用方也能打）
 */
const char *App_Audio_RetStr(int ret);

#endif /* __APP_AUDIO_H__ */
