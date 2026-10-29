/**
 * @file ds_music.h
 * @brief 后端下发的音乐数据模型（跨层共享的纯数据类型）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note ★ 为什么这个结构体放在 Common 层而不是 APP 层 ★
 *
 *       `music_t` 被两个不同层的模块同时需要：
 *         User/Middlewares/KS/ks_synth.h  —— KS 合成器要读它来合成
 *         User/APP/APP_MUSIC/app_music.h  —— 解析器要填它
 *
 *       如果把它定义在 APP 层，Middlewares 就得 REQUIRES APP；
 *       而 APP 又需要 Middlewares 里的 KS 合成器 —— 立刻形成组件循环依赖，
 *       CMake 会直接报错（组件依赖图不允许成环）。
 *
 *       常见的错误解法是「把 ks_synth 挪到 APP 层」，但那会破坏分层：
 *       合成算法是纯运算、可单测的中间件，不该依赖业务层。
 *       正确解法是把**共享的数据类型**下沉到 Common（平台无关层）：
 *          Common            ← 数据类型 + 工具（最底层，不依赖任何人）
 *            ↑          ↑
 *          APP      Middlewares
 *       Common 里只有结构体定义和几个纯 inline 换算函数，
 *       不含任何业务逻辑、不依赖任何其它组件，符合它"平台无关通用定义"的定位。
 */

#ifndef __DS_MUSIC_H__
#define __DS_MUSIC_H__

/*==============================================================================
 * 头文件包含
 *============================================================================*/
// 固定包含
#include <stdint.h>

/*==============================================================================
 * 容量常量（对齐后端上限，见 esp32-music/worker/src/music.js 的 clamp）★ 改小会截断曲子 ★
 *============================================================================*/
/** @brief 最大小节数。后端 total_bars 被 clamp 到 4~32，环境变量默认 16 */
#define MUSIC_MAX_BARS          32

/** @brief 最大和弦音总数。后端每小节最多 6 个音（buildVoicing），32×6=192 */
#define MUSIC_MAX_CHORD_NOTES   192

/** @brief 最大旋律步数。后端 melody_notes 长度 = 小节 × 每小节格数，32×8=256 */
#define MUSIC_MAX_MELODY_STEPS  256

/** @brief 最大伴奏音型长度。后端 ARP_PATTERNS 最长 16 个元素 */
#define MUSIC_MAX_ARP           16

/** @brief 鼓点网格长度：每小节固定 16 个十六分音符，最长 32 小节 */
#define MUSIC_MAX_DRUM_STEPS    512

/** @brief 后端支持的鼓点数（kick / snare / hat 三路） */
#define MUSIC_DRUM_TRACKS       3

/*==============================================================================
 * 音乐数据结构
 *============================================================================*/
/**
 * @brief 音乐数据（后端 music 字段的 C 镜像）
 *
 * @note  ★ 只保留设备真正需要的字段 ★
 *
 *        时间轴用法（见 esp32-music/docs/API.md §6）：
 *          step_duration = step_ms / 1000.0
 *          第 i 格发生时间 = 播放起点 + i × step_duration
 *
 *        伴奏（每格）：
 *          steps_per_bar = beats_per_bar × steps_per_beat
 *          bar     = i / steps_per_bar
 *          pat_idx = i % arp_len
 *          tone    = arp_pattern[pat_idx] % chord_note_counts[bar]
 *          midi    = chord_notes[offset[bar] + tone]
 *          ★ strum = 1 时不取单个音，而是把该小节所有和弦音一起拨响 ★
 *
 *        低音：每小节一个 bass_notes[bar]（后端已经低两个八度，听感有"底"）
 *
 *        旋律（每格）：
 *          melody_notes[i] < 0 表示休止；否则在该格触发
 *          melody_hold[i] 表示这个音保持多少格
 *
 *        ★ 字段来源不是 API.md，而是后端 music.js 的实际返回 ★
 *          API.md 落后于代码：实际还多出 bass_notes / strum / strum_spread_ms /
 *          damping / pick_position 和一组鼓点字段。
 */
typedef struct {
    /* ---- 时间基准 ---- */
    int16_t bpm;                    // 40~220
    int16_t key_index;              // 0=C, 1=C#, ..., 11=B
    int16_t scale_index;            // 0~7，见 API.md §4.2
    int16_t bars;                   // 4~32
    int16_t beats_per_bar;          // 默认 4
    int16_t steps_per_beat;         // 默认 2
    int16_t step_ms;                // ★ 后端已预计算，固件连除法都不用做

    /* ---- 和声 ---- */
    int16_t chord_notes[MUSIC_MAX_CHORD_NOTES];     // 所有小节的和弦音高（MIDI），扁平拼接
    int16_t chord_note_counts[MUSIC_MAX_BARS];      // 每小节各有多少个和弦音
    int16_t chord_notes_len;                        // chord_notes 实际长度
    int16_t bass_notes[MUSIC_MAX_BARS];             // 每小节一个低音（MIDI）

    /* ---- 伴奏音型 ---- */
    int16_t arp_pattern[MUSIC_MAX_ARP];             // 元素是「和弦音的下标」
    int16_t arp_len;
    int16_t strum;                                  // 1 = 扫弦（同时拨响所有和弦音）
    int16_t strum_spread_ms;                        // 扫弦时各弦错开的毫秒数（后端给 22）

    /* ---- 旋律 ---- */
    int16_t melody_notes[MUSIC_MAX_MELODY_STEPS];   // -1 = 休止
    int16_t melody_hold[MUSIC_MAX_MELODY_STEPS];    // 每个音保持多少格
    int16_t melody_len;

    /* ---- 音色（KS 合成参数）---- */
    int16_t instrument_index;       // 0~5，见 API.md §4.3
    float   decay_seconds;          // 衰减到 -60dB 的秒数
    float   brightness;             // 激励亮度 0..1
    float   damping;                // ★ 扩展 KS：环路阻尼（高频消失速度）
    float   pick_position;          // ★ 扩展 KS：拨弦位置 0..0.5

    /* ---- 鼓点（本工程暂不合成鼓，字段先收着）---- */
    int16_t drum_step_ms;           // 鼓点每格的毫秒数（每小节 16 格）
    int16_t drum_len;               // 鼓点网格长度（= bars × 16）
    uint8_t drum_kick[MUSIC_MAX_DRUM_STEPS];
    uint8_t drum_snare[MUSIC_MAX_DRUM_STEPS];
    uint8_t drum_hat[MUSIC_MAX_DRUM_STEPS];
    uint8_t drum_level[MUSIC_MAX_BARS];             // 每小节的鼓点强度 0~9
} music_t;

/*==============================================================================
 * 纯换算函数（inline，无状态）
 *============================================================================*/
/**
 * @brief 一小节有多少格
 */
static inline int Music_StepsPerBar(const music_t *m)
{
    return (int)m->beats_per_bar * (int)m->steps_per_beat;
}

/**
 * @brief 整首曲子有多少格（伴奏/鼓点的时间轴长度）
 */
static inline int Music_TotalSteps(const music_t *m)
{
    return (int)m->bars * Music_StepsPerBar(m);
}

/**
 * @brief 整首曲子的时长（毫秒）
 */
static inline uint32_t Music_DurationMs(const music_t *m)
{
    return (uint32_t)Music_TotalSteps(m) * (uint32_t)m->step_ms;
}

#endif /* __DS_MUSIC_H__ */
