/**
 * @file ks_synth.h
 * @brief Karplus–Strong 拨弦合成器（Middlewares 层，无硬件依赖）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 纯算法模块：不碰 I2S、不碰 GPIO、不依赖 ESP-IDF 的驱动，只依赖 math。
 *       上层把 music_t 喂进来，然后按块调用 KS_Synth_Render() 取 PCM。
 *
 *==============================================================================
 * ★ 与 API.md §5 的关系：文档给的是「最简版」，本实现是它的超集 ★
 *==============================================================================
 *  文档 §5.1 的反馈系数公式是对的，直接采用：
 *
 *      d = 0.001^(1/(fs × decay_seconds))
 *
 *  推导（文档已给，这里复述以便日后回看）：
 *      N = fs/频率（延迟线长度），信号每 N 个样点循环一圈，一圈乘一次 d
 *      1 秒内循环 P = fs/N = 频率 次（★ 所以「每秒循环次数 = 频率」）
 *      1 秒后总衰减 = d^P，要求 = 0.001（即 -60dB）
 *      → P × ln(d) = ln(0.001) → d = 0.001^(1/P) = 0.001^(N/fs)
 *      → d = 0.001^(1/(fs × decay_seconds))     ← 与频率无关
 *
 *  ★ 为什么这个「与频率无关」很重要：如果改成「每周期衰减系数」，
 *    低音的 N 更大 → 每秒循环次数更少 → 但每次乘的系数一样，
 *    结果低音反而衰减更慢、高音更快，和真实弦乐器相反。
 *
 *  但后端实际还下发了两个文档没写的扩展参数（来自 music.js 的 INSTRUMENTS）：
 *
 *      damping        环路阻尼：高频消失速度，独立于总延音
 *                     → 实现为延迟线环路里的一阶低通滤波器
 *      pick_position  拨弦位置 0..0.5
 *                     → 实现为激励的梳状滤波（拨弦点决定哪些谐波被强调/抵消，
 *                       这是吉他音色的关键，也是 pick 靠琴桥/靠琴颈音色不同的原因）
 *      strum + strum_spread_ms  扫弦：各弦错开 22ms
 *
 *  本实现两者都做了，所以比文档的最简版更接近后端设计的音色。
 *==============================================================================
 */

#ifndef __KS_SYNTH_H__
#define __KS_SYNTH_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>
#include <stddef.h>
#include <math.h>
// 功能包含
#include "ds_music.h"      /* music_t（Common 层共享类型） */

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
/**
 * @brief 延迟线最大长度（样点数）
 * @note  ★ 由最低音的频率决定 ★
 *       N = 采样率 / 频率。16kHz 下：
 *          C2 (65.4Hz)  → N = 245
 *          后端 bass_notes 最低是 MIDI 36 (C2, 65.4Hz)，所以 256 够；
 *          但为了以后可能加更低的音（或提高采样率），取 512。
 *       内存：48 声部 × 512 × 4 字节 = 96KB，放内部 RAM 有点多，见下面的说明。
 */
#define KS_MAX_N            512

/**
 * @brief 最大同时发声的音符数（复音数）
 * @note  需要同时容纳：一个小节的和弦（后端 buildVoicing 最多 6 个）
 *                      + 低音 1 个 + 旋律 1~2 个 + 前一小节还在衰减的音
 *                      后端 melody_hold 最多几格，音符会叠
 *        给到 32 足够，而且 32 × 512 × 4 = 64KB。
 *        ★ 如果内部 RAM 紧张，可以降到 16（会在密集处轻微丢音）★
 */
#define KS_MAX_VOICES       32

/**
 * @brief 同时发声的鼓的最大个数
 * @note  见 ks_synth_t 里 drums[] 的说明：8 个足够覆盖"上一格还没衰减完"的情况，
 *        满了就抢占最旧的（鼓是瞬态，听感上无影响）。
 */
#define KS_MAX_DRUMS        8

/**
 * @brief 每个声部的包络状态
 */
typedef enum {
    KS_VOICE_FREE = 0,      // 空闲
    KS_VOICE_ACTIVE,        // 正在发声
} KS_VoiceState_t;

/*==============================================================================
 * 类型定义
 *============================================================================*/
/**
 * @brief 单个 KS 声部
 * @note  用 float 存延迟线。虽然 int16 能省一半内存，但 KS 的反馈滤波在
 *        int16 下量化噪声会累积（每圈都舍入一次，几百圈后底噪明显），
 *        而 float 在这个规模下 CPU 完全够（16kHz 单声道，240MHz 双核）。
 */
typedef struct {
    float   buf[KS_MAX_N];      // 延迟线
    int     n;                  // 延迟线长度（整数部分，= floor(fs/频率)）
    int     idx;                // 当前读写下标
    float   d;                  // 每样点反馈系数（整体衰减，与频率无关）
    float   write_lp_a;         // 写入前的一阶低通系数（同时管阻尼 + 分数延迟）
    float   write_lp_z;         // 上述低通的状态
    float   amp;                // 声部增益包络（复音混音时的静态增益）
    int     midi;               // 该声部当前的 MIDI 音高（-1 = 未占用）
    /**
     * @brief 声部存活样点数与上限（★ 用于回收声部，审计修复 ★）
     * @note  ★ 为什么不能靠 amp 判断声部是否该回收 ★
     *        amp 是【静态增益】（和弦 0.45 / 低音 0.90 / 旋律 0.75），
     *        它从拨弦到结束都不变 —— 衰减体现在延迟线内部（buf[] 逐圈乘 d）。
     *        所以"amp < 1e-5 就回收"这个条件【永远不成立】，声部池会被
     *        32 个槽永久占满，之后每次拨弦都在抢占已有声部：
     *          用新噪声覆盖整条 buf[] → 从上一音的任意相位跳到新激励
     *          → 每抢占一次就是一声"咔"，而且 CPU 常驻满负荷。
     *        修法：用"存活了多少个样点"来决定回收，上限取
     *        一个 decay_seconds（衰减到 -60dB）+ 余量，语义上正好对应
     *        "这个音已经听不见了"。
     */
    int     life;               // 已存活样点数
    int     life_max;           // 达到该值即回收（由 decay_seconds 换算）
    uint8_t state;              // KS_VoiceState_t
} ks_voice_t;

/*==============================================================================
 * 鼓点（kick / snare / hat）
 *============================================================================*/
/**
 * @brief 鼓的三种音色
 */
typedef enum {
    KS_DRUM_KICK = 0,       /**< 底鼓：正弦波 + 音高从 120Hz 滑到 45Hz */
    KS_DRUM_SNARE,          /**< 军鼓：高通噪声 + 190Hz 三角波（鼓皮） */
    KS_DRUM_HAT,            /**< 踩镲：极短的高通噪声 */
    KS_DRUM_KIND_COUNT
} KS_DrumKind_t;

/**
 * @brief 正在发声的一个鼓
 * @note  ★ 配方来源 ★
 *        参考实现 esp32-music/worker/src/demo-page.js:555-621（playKick/playSnare/playHat）。
 *        后端注释里明确写了"三个基本音色都是几行 DSP，ESP32 上能原样照搬"，
 *        本结构就是照那个配方搬到定点/浮点 C 上的结果。
 *
 *        ★ 采样率适配（重要）★
 *        参考实现跑在浏览器 48kHz，而本工程是 16kHz（奈奎斯特 8kHz）。
 *        所以高频滤波的截止频率必须按比例下调，否则 7200Hz 的高通
 *        会几乎滤掉全部信号（16kHz 下 7200Hz 已接近奈奎斯特）：
 *            kick  正弦滑音 120→45Hz   —— 16kHz 下完全够用，不用改
 *            snare 高通 1300Hz         —— 16kHz 下没问题，不用改
 *            hat   高通 7200Hz  → 改成 4500Hz（清亮但仍可闻）
 *        各音色的包络时间与频率无关，原样保留。
 */
typedef struct {
    uint8_t  kind;          /**< 哪个音色（KS_DrumKind_t），>= KS_DRUM_KIND_COUNT 表示空闲 */
    uint8_t  level;         /**< 触发时该小节的强度 0~9（越大越响） */
    float    amp;           /**< 当前包络值 */
    float    decay;         /**< 每样点包络衰减系数 */
    float    phase;         /**< 正弦/三角相位 0~1 */
    float    phase_inc;     /**< 每样点相位增量 */
    float    freq;          /**< kick 的滑音用：当前频率 */
    float    freq_to;       /**< 目标频率 */
    float    freq_slope;    /**< 每样点频率变化量 */
    float    hp_z;          /**< 一阶高通状态（噪声整形用） */
    float    hp_a;          /**< 高通系数 */
    /**
     * @brief 鼓点后级低通状态（snare 带限用）
     * @note  ★ 为什么 snare 必须再加一级低通（实测数据）★
     *        snare 用"一阶低通取低频、再与原信号相减"的方式做高通。
     *        在 fs=16k 下 1300Hz 的一阶高通，对 Nyquist(8kHz) 的响应是 0.914，
     *        也就是说它的能量【几乎全在 8kHz】，听起来是"咔/嘶"而不是军鼓。
     *        实测加一级低通（系数 0.70，等效约 910Hz）后：
     *          最大相邻跳变  0.9139 → 0.4688（−49%，这才是"咔"的物理量）
     *          rms           0.0780 → 0.0724（仅 −7%，音量基本不变）
     *        注意"降低音量"解决不了这个问题：声音变小了，但它仍然是 8kHz 的
     *        嘶声。只有带限才真正改变音色。这两个状态变量就是这级低通的历史值。
     */
    float    lp_z1;
    float    lp_z2;
    float    lp_a;          /**< 后级低通系数（0 = 关闭） */
    /**
     * @brief 剩余存活样点数（★ 释放的时间兜底，必须有 ★）
     * @note  ★ 为什么不能只看 amp 判断是否释放（实测踩过）★
     *        amp 的释放阈值是 `amp < 1e-4`，而 amp 的初值会随小节强度放大
     *        （level=9 时初值到 1.8）。衰减系数按 0.3 秒设计，只把 1.8
     *        降到约 2.5e-4 —— **略高于阈值**，于是这个鼓永远不释放，
     *        一直占着池子（还会挤掉后来的鼓）并白烧 CPU。
     *        所以再加一个"剩余样点数"的硬上限：到点无条件释放。
     *        两个条件取先到者：衰减到听不见、或时间到。
     */
    int      life_left;
} ks_drum_t;

/**
 * @brief 合成器实例
 * @note  必须由调用方静态分配或堆分配（结构体很大，不能放栈上）：
 *        sizeof(ks_synth_t) ≈ 32 × (512×4 + 40) ≈ 66KB
 */
typedef struct {
    /* ---- 乐谱（不拷贝，只持有指针，调用方必须保证其存活）---- */
    const music_t *music;

    /* ---- 声部池 ---- */
    ks_voice_t voices[KS_MAX_VOICES];

    /* ---- 合成参数（由 music 换算，见 KS_Synth_Start）---- */
    float   sample_rate;
    float   decay_coeff;        // d，与频率无关
    float   damping;            // 0..1，环路阻尼强度
    float   brightness;         // 0..1，激励亮度
    float   pick_pos;           // 0..0.5，拨弦位置
    float   master_gain;        // 总输出增益（防止多声部叠加削顶）

    /* ---- 播放进度 ---- */
    int     step;               // 当前步
    int     total_steps;        // 总步数
    int     steps_per_bar;      // 每小节步数
    int     arp_idx;            // 音型指针
    int     chord_offset[MUSIC_MAX_BARS];   // 每小节和弦音在 chord_notes 里的起始下标
    /**
     * @brief 夹逼后的「每小节和弦音数」本地副本
     * @note  ★ 为什么不用 music->chord_note_counts 直接用 ★
     *        审计发现：chord_notes[MUSIC_MAX_CHORD_NOTES] 的读取偏移完全由
     *        counts 的前缀和决定，而解析器原先不校验元素值 ——
     *        32 小节每小节 7 个音（完全合法的输入）就会越界读，
     *        畸形响应的 32767 更是能把偏移推到 ±2MB 导致崩溃。
     *        解析器已修（App_Music_Parse 里 clamp），但合成器不该依赖
     *        「上游一定老实」，所以这里再夹一次，形成双保险：
     *          chord_offset[b] + chord_count[b] <= MUSIC_MAX_CHORD_NOTES 恒成立。
     */
    int     chord_count[MUSIC_MAX_BARS];

    /* ---- 鼓点 ---- */
    /**
     * @brief 正在发声的鼓（小池子）
     * @note  ★ 为什么 8 个就够 ★
     *        鼓点图案每格只有 0/1，而且一张 16 格图案里同时最多
     *        1 个 kick + 1 个 snare + 1 个 hat。而 kick 最长 0.3 秒、
     *        hat 只有 0.055 秒，在 125ms 一格的情况下不会堆积很多。
     *        8 个槽足以覆盖"上一格还没衰减完、下一格又来了"的情况；
     *        满了就抢占（丢弃最旧的），听感上没有影响（鼓本来就是瞬态）。
     */
    ks_drum_t drums[KS_MAX_DRUMS];
    uint32_t drum_samples;      /**< 每一格鼓点多少样点（drum_step_ms 换算） */
    int      drum_len;          /**< 鼓点图案长度（格数），0 = 没有鼓点 */
    int      drum_idx;          /**< 下一格鼓点的序号（单调递增，索引时取模） */
    uint64_t drum_next_frame;   /**< 下一格鼓点应该触发的绝对样点位置 */
    uint8_t  drums_enabled;     /**< 乐谱里有没有鼓点 */

    /* ---- 实时渲染进度 ---- */
    uint64_t frame;             // 已渲染的样点数（绝对）
    uint32_t step_samples;      // 每步多少样点
    uint32_t step_start_frame;  // 当前步的起始样点
    uint8_t  started;
    uint8_t  finished;          // 一轮放完（循环时会被重置）
} ks_synth_t;

/*==============================================================================
 * 接口
 *============================================================================*/
/**
 * @brief 初始化合成器（绑定乐谱并换算参数）
 * @param s     合成器实例（调用方分配，建议用 PSRAM 或静态区，不要放栈上）
 * @param music 乐谱（合成期间必须保持存活）
 * @param sample_rate 采样率（用 BSP_I2S_SAMPLE_RATE = 16000）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @note  只做参数换算和清零，不做任何动态分配。
 */
int KS_Synth_Init(ks_synth_t *s, const music_t *music, uint32_t sample_rate);

/**
 * @brief 从头开始播放（重置进度与所有声部）
 * @param s 合成器实例
 * @retval 0: 成功
 * @retval -1: 参数错误/未初始化
 * @note  ★ 需求里「再次按下停止，下一次播放从头开始」就靠这个函数实现 ★
 */
int KS_Synth_Restart(ks_synth_t *s);

/**
 * @brief 立即停止所有发声（不重置进度）
 * @param s 合成器实例
 * @note  把每个声部的 amp 设为 0 会让它们立刻静音；
 *        但直接清零会产生"咔"声，所以本函数做成短释放包络（见 .c）。
 */
void KS_Synth_Stop(ks_synth_t *s);

/**
 * @brief 触发一个音符（拨弦）
 * @param s    合成器实例
 * @param midi MIDI 音高（<0 忽略）
 * @param amp  增益（0..1，和弦按 0.6、低音按 0.8、旋律按 0.9 之类）
 * @retval 0: 成功
 * @retval -1: 参数错误
 * @retval -2: 没有空闲声部（会抢占最早发声的那个）
 */
int KS_Synth_Pluck(ks_synth_t *s, int midi, float amp);

/**
 * @brief 渲染一块 PCM
 * @param s     合成器实例
 * @param out   输出缓冲（int16 单声道）
 * @param frames 期望渲染多少样点
 * @retval 实际渲染的样点数（正常情况下 = frames）
 * @note  ★ 这个函数会按 frame 进度自动触发伴奏/低音/旋律的音符 ★
 *        所以上层只需要反复调它往 I2S 里灌就行，不用管时间轴。
 *        时间轴由 frame 和 step_samples 精确控制：
 *          第 k 步的起始样点 = k × step_samples
 *        这样即使渲染块大小不均匀，节拍也不会漂。
 */
int KS_Synth_Render(ks_synth_t *s, int16_t *out, size_t frames);

/**
 * @brief 一轮是否已播完（用于循环判断）
 * @param s 合成器实例
 * @retval 1: 已播完
 * @retval 0: 还在播
 */
uint8_t KS_Synth_IsFinished(const ks_synth_t *s);

/**
 * @brief 当前还有多少个声部在发声
 * @param s 合成器实例
 * @retval 处于 ACTIVE 状态的声部数量（0 = 全部静音）
 *
 * @note  ★ 用于判断"一轮是否真的放完了"★
 *        `KS_Synth_IsFinished()` 只表示"不再触发新音符"，此时已有声部还在
 *        自然衰减（decay_seconds 最长 3.5 秒）。循环播放要在
 *        `IsFinished() && ActiveVoices()==0` 时才能重绕，否则会把尾音硬切
 *        → 循环接缝处"咔"一声。
 */
uint32_t KS_Synth_ActiveVoices(const ks_synth_t *s);

/**
 * @brief 手动触发一个鼓（调试用；正常播放由 Render 内部按图案自动触发）
 * @param s     合成器实例
 * @param kind  音色（KS_DRUM_KICK / SNARE / HAT）
 * @param level 强度 0~9（0 = 不发声）
 * @retval  0: 已触发
 * @retval -1: 参数错误或合成器未就绪
 */
int KS_Synth_TriggerDrum(ks_synth_t *s, KS_DrumKind_t kind, uint8_t level);

/**
 * @brief 当前正在发声的鼓的个数
 */
uint32_t KS_Synth_ActiveDrums(const ks_synth_t *s);

/**
 * @brief MIDI 音高转频率
 * @note  440Hz = A4 = MIDI 69。用 exp2f 而不是 powf(2, x)：
 *        exp2f 是单指令级的实现，powf 要走通用幂运算，快很多。
 */
static inline float KS_MidiToFreq(float midi)
{
    return 440.0f * exp2f((midi - 69.0f) / 12.0f);
}

#endif /* __KS_SYNTH_H__ */
