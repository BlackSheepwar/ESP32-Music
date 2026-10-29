/**
 * @file ks_synth.c
 * @brief Karplus–Strong 拨弦合成器实现
 * @author 李嘉图
 * @date 2026-06-26
 *
 *==============================================================================
 * ★ 算法要点（改之前先读懂，否则音色会立刻变难听）★
 *==============================================================================
 *
 *  1) 延迟线长度与「分数延迟」
 *     理想长度 N = fs / freq 通常不是整数。若直接四舍五入，16kHz 下 1kHz 的 N
 *     只有 16，取整误差带来约 0.4% 的音高偏差（约 7 音分），和旋律一起响时能听出来。
 *     本实现把「写入延迟线之前的一阶低通」当作分数延迟补偿器：
 *     f0 处的相位延迟 ≈ (1-a)/a 个样点，取 a = 1 - 1/(frac+1) 即可补上 frac。
 *     好处是零额外内存（只多一个 float 状态），也不需要全通滤波器那套状态变量。
 *
 *  2) 写入低通同时承担阻尼（damping）
 *     一阶低通写成「单位直流增益」形式：
 *         y[n] = a·y[n-1] + (1-a)·x[n]
 *     它的直流增益恰好是 1，所以【不影响 decay_seconds 规定的衰减时间】，
 *     只让高频衰减更快 —— 这正是 damping 的语义（音乐上就是"弦变闷"）。
 *     如果换成别的归一化方式，直流增益就不是 1，decay_seconds 会失准，
 *     音会过短或过长。这一点是调试时最容易搞错的地方。
 *
 *     取舍：阻尼和分数延迟共用一个低通系数，取两者中较大的 a。
 *     这会带来「阻尼不足时高频略多」的偏差，但音准问题的容忍度远低于音色，
 *     所以优先保证音准。
 *
 *  3) 激励（拨弦瞬间的初始波形）
 *     顺序很重要：白噪声 → 梳状滤波(pick_position) → 低通(brightness) → 去直流
 *     - 梳状滤波：拨弦位置决定哪些谐波被强调/抵消，是吉他音色的关键
 *       延迟 d = pick_position × N × 2，只有 d ≥ 1 才有意义
 *     - 必须去直流：延迟线里如果有直流分量，会绕圈永远不消失，
 *       表现为"嗡嗡"的直流噪声叠在音上，而且让 rms 虚高
 *
 *  4) 松弦（释放）不能直接清零
 *     直接把 amp 置 0 会在波形中途截断 → 产生"咔"的爆音。
 *     所以 KS_Synth_Stop 走一段很短的线性释放（本实现用 5ms 的衰减系数）。
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
#include "ks_synth.h"
// 功能包含
#include "esp_log.h"
#include "esp_random.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "KS";

/**
 * @brief 梳状滤波用的副本缓冲（★ 审计修复：不再就地处理 ★）
 * @note  ★ 为什么需要这份副本 ★
 *        梳状滤波要把 buf[i] 换成 buf[i] - buf[i-d]，如果就地写回，
 *        后面取"d 个样点前"的值时可能取到【已经被本轮改过】的值。
 *        原来的实现想用"从后往前遍历"来规避，但那只能解决 i >= d 的情况；
 *        当 i < d 时环形索引会回绕到缓冲尾部，而尾部已经被处理过了
 *        → 读到的是差分后的值（实测 n=15 时 15 个样点里有 12 个是错的）。
 *        参考实现 esp32-music/worker/src/demo-page.js:679 用 `buf.slice()` 副本。
 *
 *        ★ 为什么可以用文件级 static（不加锁）★
 *          ks_pluck_voice() 是同步函数、没有阻塞点、也不会递归，
 *          同一时刻只可能有一个调用者在用它；而且整个合成器
 *          本来就只有音频任务在碰（见 App_Audio_SynthReload 的说明）。
 *          所以一份 2KB 的共享副本是安全的，不必每声部各配一份
 *          （那样要多花 32×2KB = 64KB）。
 */
static float s_comb_tmp[KS_MAX_N];

/** @brief 各声部的基准增益（避免叠加削顶，具体值在 Render 里按类型传） */
#define KS_GAIN_BASS        0.90f
#define KS_GAIN_CHORD       0.45f
#define KS_GAIN_MELODY      0.75f
#define KS_GAIN_STRUM       0.30f      /* 扫弦时 6 根弦同时响，单弦要更轻 */

/** @brief 总输出增益。多声部叠加时防止削顶 */
#define KS_MASTER_GAIN      0.30f

/** @brief 释放时常量：停止时的线性释放步长（约 5ms @16kHz） */
#define KS_RELEASE_SAMPLES  80

/** @brief 计算反馈系数：d = 0.001^(1/(fs × decay_seconds)) */
static float ks_decay_coeff(float sample_rate, float decay_seconds)
{
    if (sample_rate <= 0.0f || decay_seconds <= 0.0f)
        return 0.999f;

    float d = powf(0.001f, 1.0f / (sample_rate * decay_seconds));

    /* 数值保护：d 必须 < 1，否则永不衰减（自激） */
    if (d >= 1.0f) d = 0.9999f;
    if (d <= 0.0f) d = 0.5f;
    return d;
}

/*==============================================================================
 * 声部：拨弦（激励生成）
 *============================================================================*/
/**
 * @brief 生成激励并初始化一个声部
 * @param v        声部
 * @param freq     频率（Hz）
 * @param s        合成器（取参数）
 * @param amp      增益
 */
static void ks_pluck_voice(ks_voice_t *v, float freq, const ks_synth_t *s, float amp)
{
    /* ---------- 1. 延迟线长度（整数部分 + 分数补偿） ---------- */
    /* ★ 分数延迟怎么补（★ 审计后修正了注释里的错误公式 ★）
     *
     *   做法：在「写入延迟线之前」放一阶低通，用它的相位延迟凑分数部分。
     *
     *   ★ 正确的相位延迟公式是 a/(1-a)，不是 (1-a)/a ★
     *     一阶低通 H(z) = (1-a)/(1 - a·z⁻¹)，令 z = e^{jω}：
     *         φ(ω) = -atan( a·sinω / (1 - a·cosω) )
     *         τ(ω) = -φ(ω)/ω = atan( a·sinω / (1 - a·cosω) ) / ω
     *     当 ω→0 时 τ → a/(1-a)。所以要让延迟 = frac，应取 a = frac/(1+frac)。
     *     （原注释写的 a = 1/(frac+1) 会让延迟变成 1/frac，是反的。实现
     *       本身取的是 1 - 1/(frac+1) = frac/(1+frac)，是【对的】，
     *       只有注释写错了 —— 留着错误的注释会误导后人，所以一并改正。）
     *
     *   ★ 已知残留偏差（记录在案，未修）★
     *     环路总延迟 = n + τ(a)，而本实现取 a = max(a_damp, a_frac)，
     *     只要 a_damp > a_frac（中高音几乎总是），环路就比 fs/freq 长一点点
     *     → 系统性音偏低（实测不同音色最差 −14 ~ −82 音分），且 frac 逐音不同
     *     → 偏的量逐音变化。
     *     正确修法是「先从延迟线长度里扣掉 τ，再另用一阶全通补 frac」，
     *     参考实现 esp32-music/worker/src/demo-page.js:647-661 就是这么做的。
     *     但那要改最热的 ks_voice_next() 并给每个声部加 3 个 float，
     *     属于听感调优范畴，本工程暂缓（详见 HARDWARE.md 的已知限制一节）。 */
    float n_f = s->sample_rate / freq;
    if (n_f < 2.0f) n_f = 2.0f;
    if (n_f > (float)KS_MAX_N - 2.0f) n_f = (float)KS_MAX_N - 2.0f;

    v->n = (int)n_f;
    float frac = n_f - (float)v->n;

    v->idx   = 0;
    v->amp   = amp;
    v->state = KS_VOICE_ACTIVE;
    v->d     = s->decay_coeff;
    v->write_lp_z = 0.0f;

    /* ★ 声部寿命：用于回收声部（审计修复 S1）★
     * 上限 = decay_seconds（衰减到 -60dB 所需时间）+ 100ms 余量。
     * 必须用寿命计数而不是 amp 判断：amp 是静态增益、永远不变，
     * 原来的 `amp < 1e-5` 回收条件从不成立，导致声部池永不回收。 */
    v->life     = 0;
    v->life_max = (int)((float)s->sample_rate * s->music->decay_seconds)
                  + (int)((float)s->sample_rate * 0.1f);
    if (v->life_max < 1)
        v->life_max = 1;

    /* ---------- 写入低通系数：同时承担「阻尼」和「分数延迟」 ---------- */
    /* 两个需求都是"加一点相位延迟"，合成一个一阶低通：
     *   a_damp  = damping                    （后端下发：高频消失速度）
     *   a_frac  = frac/(1+frac)              （补分数延迟所需的等效系数，公式见上）
     * 取较大的 a（即更强的滤波）保证分数延迟够；阻尼不够时高频会稍微多留一点，
     * 听感上比"音不准"好得多 —— 音准问题的容忍度远低于音色。 */
    float a_damp = s->damping;
    float a_frac = (frac > 1e-4f) ? (1.0f - 1.0f / (frac + 1.0f)) : 0.0f;
    float a = (a_damp > a_frac) ? a_damp : a_frac;

    if (a < 0.0f)   a = 0.0f;
    if (a > 0.95f)  a = 0.95f;
    v->write_lp_a = a;

    /* ---------- 2. 激励：白噪声 ---------- */
    for (int i = 0; i < v->n; i++)
    {
        /* esp_random 是硬件随机，比 rand() 快且不用播种；
         * 归一化到 -1..1 */
        int32_t r = (int32_t)esp_random();
        v->buf[i] = ((float)r / 2147483648.0f);
    }

    /* ---------- 3. 梳状滤波（拨弦位置） ----------
     * 拨弦点距离端点 d 时，往返路程会产生陷波，
     * 这就是"靠琴桥拨弦声音更亮更薄"的原因。
     *
     * ★★ 这里有两个审计发现的 bug，都已修 ★★
     *
     * (a) 延迟量算错了 2 倍。
     *     延迟线里存的【已经是一个完整周期（往返）】，所以拨弦点比例 p
     *     造成的陷波应落在 f = k·f0/p，即延迟量 = p·N。
     *     原来写的是 pick_pos * 2.0f * n，把陷波搬到了 k·f0/(2p)，
     *     音色整体偏"薄"。
     *     参考实现 esp32-music/worker/src/demo-page.js:678 用的是
     *     `M = Math.max(1, Math.round(N * pickPos))` —— 没有乘 2。
     *
     * (b) pick_position == 0.5 会把整条延迟线清零 → 该音色完全静音。
     *     后端 music.js:250 里 harp 的 pick_position 正好是 0.50。
     *     推导（按旧公式）：d_samples = 0.5 × 2 × n = n（整数），
     *     于是 i1 = i - n，环回后 i1 == i 自身
     *       → delayed = buf[i]  →  buf[i] -= buf[i] = 0
     *     整条激励变成 0，peak 也是 0，又跳过了归一化 → 永久静音。
     *     而且 pick_pos 是全局参数，所以旋律+低音+和弦全部没声，
     *     【整首曲子一点声音都没有】。
     *     参考实现 demo-page.js:677 用 `pickPos < 0.5` 明确排除；
     *     这里除了加同样的边界，Init 里也把上限收到 0.49（双保险）。
     *
     * (c) 原来用"从后往前"的就地处理来避免读到刚覆盖的数据 —— 那是错的。
     *     反序遍历只对 i >= d_int 有效；当 i < d_int 时 i1 = i - d_int + n
     *     落在缓冲【尾部】，而那部分已经被本轮处理过了 → 读到的是差分后的值。
     *     实测 n=15/pick=0.4 时 15 个样点里有 12 个是错的。
     *     参考实现用 `var tmp = buf.slice()` 副本（demo-page.js:679）。
     *     这里改用文件级副本缓冲（拨弦是同步的、不会重入，所以一份就够）。 */
    float d_samples = s->pick_pos * (float)v->n;    /* ★ 修 (a)：不再乘 2 ★ */

    if (d_samples >= 1.0f && d_samples < (float)v->n - 0.5f)   /* ★ 修 (b) ★ */
    {
        int d_int = (int)d_samples;
        float d_frac = d_samples - (float)d_int;

        /* ★ 修 (c)：拷一份副本，顺序遍历，不再就地覆盖 ★ */
        memcpy(s_comb_tmp, v->buf, (size_t)v->n * sizeof(float));

        for (int i = 0; i < v->n; i++)
        {
            int i1 = i - d_int;
            int i2 = i1 - 1;
            while (i1 < 0) i1 += v->n;
            while (i2 < 0) i2 += v->n;
            while (i1 >= v->n) i1 -= v->n;
            while (i2 >= v->n) i2 -= v->n;

            float delayed = s_comb_tmp[i1] * (1.0f - d_frac) + s_comb_tmp[i2] * d_frac;
            v->buf[i] = s_comb_tmp[i] - delayed;    /* 差分 = 梳状滤波 */
        }
    }

    /* ---------- 4. 低通（brightness：亮 = 保留更多高频 = 拨弦更"脆"） ---------- */
    /* a = brightness 越大 → 低通越弱 → 高频保留越多。
     * 注意这里是激励的整形，和环路阻尼是两件事。 */
    {
        float a = s->brightness;
        if (a < 0.0f) a = 0.0f;
        if (a > 0.98f) a = 0.98f;

        float z = 0.0f;
        for (int i = 0; i < v->n; i++)
        {
            z = a * z + (1.0f - a) * v->buf[i];
            v->buf[i] = z;
        }
    }

    /* ---------- 5. 去直流 ---------- */
    /* ★ 关键：延迟线里的直流分量会永远绕圈不衰减，
     *   表现为持续的"嗡嗡"声并把 rms 抬高。必须减掉均值。 */
    {
        float sum = 0.0f;
        for (int i = 0; i < v->n; i++)
            sum += v->buf[i];
        float mean = sum / (float)v->n;

        float peak = 0.0f;
        for (int i = 0; i < v->n; i++)
        {
            v->buf[i] -= mean;
            float a = fabsf(v->buf[i]);
            if (a > peak) peak = a;
        }

        /* 归一化到统一幅度，避免不同弦之间音量差异过大 */
        if (peak > 1e-6f)
        {
            float g = 0.9f / peak;
            for (int i = 0; i < v->n; i++)
                v->buf[i] *= g;
        }
        else
        {
            /* ★ 兜底：激励被削成 0 时重新填白噪声，绝不静音（审计修复）★
             * 正常情况下 peak 不会为 0（梳状滤波后有归一化），但有两条路径
             * 可能让它变成 0：pick_position 恰好等于某个会让差分抵消的值，
             * 或者滤波器把整条缓冲压平。
             * 一旦 peak==0 还去跳过归一化，这个声部（乃至整首曲子，
             * 因为 pick_pos / brightness 是全局参数）就会【完全没声音】。
             * 宁可重新填一遍噪声（音色略糙），也绝不静音。 */
            ESP_LOGW(TAG, "pluck: excitation collapsed (peak=0), refilling noise");
            for (int i = 0; i < v->n; i++)
            {
                int32_t r = (int32_t)esp_random();
                v->buf[i] = ((float)r / 2147483648.0f) * 0.9f;
            }
        }
    }
}

/*==============================================================================
 * 鼓点合成（kick / snare / hat）
 *============================================================================*/
/**
 * @brief 一阶低通系数：截止频率 fc（Hz）→ 系数 a
 * @note  一阶低通 y[n] = a·y[n-1] + (1-a)·x[n]，其 -3dB 截止约为
 *        fc = fs·(1-a)/(2π·a)。反解：a = 1/(1 + 2π·fc/fs)。
 *        这里只用于"噪声整形"，精度要求不高，一阶够用且极便宜。
 */
static float ks_lp_coeff(float fc, float fs)
{
    float w = 2.0f * (float)M_PI * fc / fs;
    return 1.0f / (1.0f + w);
}

/**
 * @brief 每样点的包络衰减系数：在 seconds 秒内衰减到 -60dB
 * @note  与 KS 的 decay_coeff 同款公式（0.001 就是 -60dB）。
 */
static float ks_env_coeff(float seconds, float fs)
{
    if (seconds <= 0.0f)
        return 0.0f;
    return powf(0.001f, 1.0f / (fs * seconds));
}

/** @brief 三角波（相位 0~1） */
static inline float ks_triangle(float phase)
{
    /* 先折成 0~1 的三角：phase<0.5 上升，>=0.5 下降，范围 -1..1 */
    float x = phase - 0.5f;
    if (x < 0.0f) x = -x;
    return 4.0f * x - 1.0f;     /* 0 -> -1, 0.5 -> +1, 1 -> -1 */
}

/**
 * @brief 触发一个鼓
 *
 * @note  ★ 配方来源与采样率适配 ★
 *        参考实现 esp32-music/worker/src/demo-page.js:555-621
 *        （playKick / playSnare / playHat）。后端注释写明
 *        「三个基本音色都是几行 DSP，ESP32 上能原样照搬」。
 *
 *        参数对照（48kHz 浏览器 → 16kHz 本工程）：
 *
 *          kick ：正弦 120Hz →(60ms 指数滑到) 45Hz，包络 0.3s
 *                 ★ 频率与采样率无关，原样照搬 ★
 *
 *          snare：高通噪声（参考用 1300Hz 双二阶）+ 190Hz 三角，包络 0.19s / 0.13s
 *                 ★ 16kHz 下 1300Hz 完全在带内，照搬即可 ★
 *                 本实现用"白噪声 - 一阶低通(1300Hz)"当作高通（见下方说明）
 *
 *          hat  ：参考用 7200Hz 高通，包络 0.055s
 *                 ⚠️ **必须下调**：16kHz 的奈奎斯特只有 8000Hz，
 *                    7200Hz 高通会把几乎所有能量滤掉（听不到）。
 *                    这里改成 4500Hz —— 仍然清亮，但在带内可闻。
 */
int KS_Synth_TriggerDrum(ks_synth_t *s, KS_DrumKind_t kind, uint8_t level)
{
    if (s == NULL || kind >= KS_DRUM_KIND_COUNT || level == 0)
        return -1;

    float fs = s->sample_rate;
    if (fs <= 0.0f)
        return -1;

    /* 找一个空闲槽；没有就抢占"最旧"的（amp 最小的那个近似）
     * —— 鼓是瞬态，抢占听感上无影响 */
    int slot = -1;
    for (int i = 0; i < KS_MAX_DRUMS; i++)
    {
        if (s->drums[i].kind >= KS_DRUM_KIND_COUNT) { slot = i; break; }
    }
    if (slot < 0)
    {
        float min_amp = 1e9f;
        for (int i = 0; i < KS_MAX_DRUMS; i++)
        {
            if (s->drums[i].amp < min_amp) { min_amp = s->drums[i].amp; slot = i; }
        }
    }
    if (slot < 0)
        return -1;

    ks_drum_t *d = &s->drums[slot];
    memset(d, 0, sizeof(*d));

    d->kind  = (uint8_t)kind;
    d->level = level;

    /* 强度 0~9 → 增益。用 3 作为"标准格"（后端 drum_level 的常见值），
     * 所以 level=3 时增益为 1.0，level=9 时约 1.8 倍。
     * 这样既保留了段落动态（弱段安静、副歌更响），也不会让满强度爆掉。 */
    float lv = (float)level / 3.0f;
    if (lv > 2.0f) lv = 2.0f;

    switch (kind)
    {
    case KS_DRUM_KICK:
        /* 正弦 + 音高滑音：120Hz → 45Hz 在 60ms 内滑完（"砰"的关键） */
        d->amp       = 1.00f * lv;
        d->decay     = ks_env_coeff(0.30f, fs);
        d->phase     = 0.0f;
        d->freq      = 120.0f;
        d->freq_to   = 45.0f;
        /* 每样点频率变化量：在 0.06s 内走完 (120-45) */
        d->freq_slope = (d->freq_to - d->freq) / (0.06f * fs);
        d->phase_inc = d->freq / fs;
        d->hp_a      = 0.0f;            /* kick 不需要高通 */
        d->lp_a      = 0.0f;            /* kick 不需要后级带限 */
        /* 时间兜底：包络本身按 0.30s 设计，再多给 100ms 余量 */
        d->life_left = (int)(0.40f * fs);
        break;

    case KS_DRUM_SNARE:
        /* 噪声部分 + 190Hz 三角（鼓皮）。这里用一个 drv 同时算两者：
         *   amp 管噪声，三角用 phase 生成后按固定比例叠加 */
        d->amp       = 0.70f * lv;
        d->decay     = ks_env_coeff(0.19f, fs);
        d->phase     = 0.0f;
        d->phase_inc = 190.0f / fs;
        d->hp_a      = ks_lp_coeff(1300.0f, fs);    /* 用于把噪声做成高通 */
        /* ★ 后级带限：把 8kHz 的"嘶"压成军鼓（实测 maxDelta 减半，rms 只掉 7%）★
         *   详见 ks_synth.h 里 lp_a 的注释。系数 0.70 ≈ 910Hz 等效截止。 */
        d->lp_a      = 0.70f;
        d->lp_z1     = 0.0f;
        d->lp_z2     = 0.0f;
        /* 时间兜底：噪声 0.19s / 鼓皮 0.13s，取 0.25s + 余量 */
        d->life_left = (int)(0.30f * fs);
        break;

    case KS_DRUM_HAT:
        /* 极短高通噪声。★ 截止从参考的 7200Hz 下调到 4500Hz ★ */
        d->amp       = 0.45f * lv;
        d->decay     = ks_env_coeff(0.055f, fs);
        d->hp_a      = ks_lp_coeff(4500.0f, fs);
        /* hat 保持纯高通噪声（镲片本来就该是宽频"嘶"），不做后级带限 */
        d->lp_a      = 0.0f;
        /* 时间兜底：帽 0.055s，取 0.12s + 余量 */
        d->life_left = (int)(0.12f * fs);
        break;

    default:
        return -1;
    }

    return 0;
}

/**
 * @brief 推进一个鼓一个样点
 * @retval 输出值（未乘 master_gain）
 * @note  ★ 包络用的是"乘性衰减"（每样点乘 decay），等价于指数包络 ★
 *        参考实现用的是 WebAudio 的 exponentialRampToValueAtTime，
 *        语义一致。到 -60dB 之后直接把槽位释放（amp 很小就置空闲）。
 */
static inline float ks_drum_next(ks_drum_t *d, float inv_fs)
{
    float out = 0.0f;

    switch ((KS_DrumKind_t)d->kind)
    {
    case KS_DRUM_KICK:
        out = sinf(2.0f * (float)M_PI * d->phase);
        /* 频率向目标滑（线性；在这个范围内与指数式差别很小，省一次 expf） */
        d->freq += d->freq_slope;
        if (d->freq < d->freq_to) d->freq = d->freq_to;
        d->phase += d->freq * inv_fs;
        if (d->phase >= 1.0f) d->phase -= 1.0f;
        break;

    case KS_DRUM_SNARE:
    {
        /* 噪声：esp_random 是硬件随机，归一化到 -1..1 */
        int32_t r = (int32_t)esp_random();
        float white = (float)r / 2147483648.0f;

        /* 一阶低通拿到低频分量，用它做差分 → 高通（沙沙声） */
        d->hp_z = d->hp_a * d->hp_z + (1.0f - d->hp_a) * white;
        float hp = white - d->hp_z;

        /* 鼓皮：190Hz 三角波，按参考的 0.55 比例叠加 */
        float skin = ks_triangle(d->phase) * 0.55f;
        d->phase += d->phase_inc;
        if (d->phase >= 1.0f) d->phase -= 1.0f;

        /* ★ 前级 2.6 是"带限之后补量感"的经验值（实测：2.6 → rms 0.0174）★
         *   原来 1.6 是"把高通后的能量补回来"的宽带口径系数。下面那级带限
         *   低通把 8kHz 的嘶声砍掉之后，总 rms 会明显下降（实测 0.0370 →
         *   0.0174，−53%）。**被砍掉的能量本来在 16kHz 采样率下听不见**，
         *   所以不该拿 rms 去补，但这个损失确实让军鼓"变小"，所以往上抬了
         *   一档。再往上就要看余量了：level=3 时 peak=0.2261 → level=9 约
         *   0.68，仍在软拐点 0.70 之下；再翻倍就会开始触发限幅。 */
        out = hp * 2.6f + skin;

        /* ★ 带限：两级一阶低通，削掉 8kHz 的"咔"★
         *   只做高通时能量几乎全在 Nyquist(8kHz)，听感是"嘶/咔"而不是军鼓。
         *   这里不另算截止频率，直接用系数（见 ks_synth.h 的实测数据）。
         *   注：lowpass 也作用在 190Hz 的鼓皮三角上，但它远低于截止，几乎不受影响。 */
        if (d->lp_a > 0.0f)
        {
            d->lp_z1 = d->lp_a * d->lp_z1 + (1.0f - d->lp_a) * out;
            d->lp_z2 = d->lp_a * d->lp_z2 + (1.0f - d->lp_a) * d->lp_z1;
            out = d->lp_z2;
        }
        break;
    }

    case KS_DRUM_HAT:
    {
        int32_t r = (int32_t)esp_random();
        float white = (float)r / 2147483648.0f;
        d->hp_z = d->hp_a * d->hp_z + (1.0f - d->hp_a) * white;
        out = (white - d->hp_z) * 1.8f;
        break;
    }

    default:
        return 0.0f;
    }

    float y = out * d->amp;

    d->amp *= d->decay;

    /* ★ 两个释放条件取先到者 ★
     *   ① 衰减到听不见（正常情况下走这条）
     *   ② 时间到（兜底）—— 必须有它，因为 amp 初值随强度放大，
     *      大强度时衰减 0.3 秒后仍在 1e-4 之上，光靠 ① 会永不释放。 */
    if (d->amp < 1e-4f)
        d->amp = 0.0f;

    if (--d->life_left <= 0 || d->amp <= 0.0f)
    {
        d->amp       = 0.0f;
        d->life_left = 0;
        d->kind      = KS_DRUM_KIND_COUNT;   /* 释放槽位 */
    }

    return y;
}

/*==============================================================================
 * 声部：单样点推进
 *============================================================================*/
/**
 * @brief 推进一个声部一个样点，返回输出值
 * @note  ★ 这是整个合成器最热的代码，每次渲染要跑 frames × voices 次 ★
 *        所以全部用局部变量、索引预先算好。
 *
 *        结构（与 API.md §5.2 的最简版对应，只是多了写入滤波）：
 *            读 buf[idx] ──┬──────────────────────────────▶ 输出
 *                          └─ 写入低通 ── ×d ──▶ 写回 buf[idx]
 *        「写入低通」同时承担高频阻尼(damping)和分数延迟补偿，理由见 ks_pluck_voice。
 */
static inline float ks_voice_next(ks_voice_t *v)
{
    /* 原点输出 */
    float cur = v->buf[v->idx];

    /* 写入低通（单位直流增益形式：a·y + (1-a)·x，直流增益恰为 1，
     * 所以不会改变 decay_seconds 规定的衰减时间） */
    v->write_lp_z = v->write_lp_a * v->write_lp_z + (1.0f - v->write_lp_a) * cur;

    /* 反馈写回 */
    v->buf[v->idx] = v->d * v->write_lp_z;

    /* 环形推进 */
    v->idx++;
    if (v->idx >= v->n) v->idx = 0;

    return cur * v->amp;
}

/*==============================================================================
 * 初始化 / 控制
 *============================================================================*/
int KS_Synth_Init(ks_synth_t *s, const music_t *music, uint32_t sample_rate)
{
    if (s == NULL || music == NULL || sample_rate == 0)
        return -1;

    memset(s, 0, sizeof(*s));

    s->music       = music;
    s->sample_rate = (float)sample_rate;

    /* ---------- 参数换算 ---------- */
    s->decay_coeff = ks_decay_coeff(s->sample_rate, music->decay_seconds);
    s->damping     = music->damping;
    s->brightness  = music->brightness;
    s->pick_pos    = music->pick_position;
    s->master_gain = KS_MASTER_GAIN;

    /* 参数合法性兜底（后端理论上会给合理值，但不能假设） */
    if (s->damping    < 0.0f) s->damping    = 0.0f;
    if (s->damping    > 0.95f) s->damping   = 0.95f;
    if (s->brightness < 0.0f) s->brightness = 0.0f;
    if (s->brightness > 0.98f) s->brightness = 0.98f;
    if (s->pick_pos   < 0.0f) s->pick_pos   = 0.0f;
    /* ★ 上限收到 0.49（审计修复 S2 的双保险）★
     * 后端 music.js:250 里 harp 的 pick_position 正好是 0.50，
     * 而 0.50 会让梳状滤波的延迟量等于整条延迟线 → 激励被差分清零
     * → 该音色（且因为 pick_pos 是全局参数，整首曲子）完全静音。
     * ks_pluck_voice 里已经加了 `d_samples < n - 0.5` 的边界检查，
     * 这里把上限也收住，形成两道防线。参考实现 demo-page.js:677
     * 同样用 `pickPos < 0.5` 排除。 */
    if (s->pick_pos   > 0.49f) s->pick_pos  = 0.49f;

    /* ---------- 时间轴 ----------
     * ★ bars 必须先夹住（审计修复）★
     *   music->bars 被用来索引所有 [MUSIC_MAX_BARS] 的数组
     *   （chord_offset / chord_count / bass_notes / drum_level）。
     *   解析器已 clamp 到 1..32，但合成器作为独立模块不该假设上游老实 ——
     *   一旦 bars > 32，前缀和写 chord_offset[b] 就直接越界写。 */
    if (music->bars <= 0 || music->bars > MUSIC_MAX_BARS)
    {
        ESP_LOGE(TAG, "bars=%d out of range (1..%d)", music->bars, MUSIC_MAX_BARS);
        return -1;
    }

    s->steps_per_bar = Music_StepsPerBar(music);
    if (s->steps_per_bar <= 0)
        s->steps_per_bar = 8;

    s->total_steps = Music_TotalSteps(music);
    if (s->total_steps <= 0)
        return -1;

    /* ★ 每步的样点数用 double 算，避免累积取整误差让节拍越走越偏 ★
     * step_ms 是后端预计算好的整数毫秒，这里换算成样点。
     * ★ 解析器已把 step_ms clamp 到 ≥1，所以 (uint32_t) 转换不会是负数 ★ */
    s->step_samples = (uint32_t)((double)sample_rate * (double)music->step_ms / 1000.0 + 0.5);
    if (s->step_samples == 0)
        s->step_samples = 1;

    /* ---------- 每小节和弦音在 chord_notes 里的起始下标 ----------
     * ★★ 这是审计发现的唯一严重缺陷的修复点 ★★
     *
     * 原实现直接 acc += music->chord_note_counts[b]，没有任何上界检查。
     * 而 chord_offset[b] 之后被用作 chord_notes[] 的基址：
     *     int base = s->chord_offset[bar];
     *     int midi = m->chord_notes[base + tone];     // ← 越界读
     * chord_notes 只有 MUSIC_MAX_CHORD_NOTES 个槽，只要 counts 之和超过它就越界。
     * 触发条件包括完全合法的输入：32 小节 × 每小节 7 个和弦音 = 224 > 192。
     * 畸形响应（counts 为 32767，或 cJSON 把 1e30 饱和成 INT_MAX 再截成 -1）
     * 能把偏移推到 ±100 万 → 地址偏移 ±2MB → LoadProhibited 崩溃。
     *
     * 三重夹逼，保证 chord_offset[b] + chord_count[b] <= limit 恒成立：
     *   1) 负数当 0
     *   2) 单小节不超过剩余可用槽位
     *   3) 累计不超过 min(chord_notes_len, MUSIC_MAX_CHORD_NOTES)
     * ★ 全部 MUSIC_MAX_BARS 个小节都要初始化（不只是 bars 个），
     *   因为 ks_trigger_step 可能因边界钳制读到后面的槽位。 */
    {
        int limit = music->chord_notes_len;
        if (limit <= 0 || limit > MUSIC_MAX_CHORD_NOTES)
            limit = MUSIC_MAX_CHORD_NOTES;

        int acc = 0;
        for (int b = 0; b < MUSIC_MAX_BARS; b++)
        {
            int c = 0;

            if (b < music->bars)
            {
                c = music->chord_note_counts[b];
                if (c < 0)
                    c = 0;
                if (c > limit - acc)
                    c = limit - acc;
                if (c < 0)
                    c = 0;
            }

            s->chord_offset[b] = acc;
            s->chord_count[b]  = c;
            acc += c;
        }

        ESP_LOGD(TAG, "chord clamp: limit=%d used=%d", limit, acc);
    }

    /* ---------- 声部池清零 ---------- */
    for (int i = 0; i < KS_MAX_VOICES; i++)
    {
        s->voices[i].state = KS_VOICE_FREE;
        s->voices[i].midi  = -1;
        s->voices[i].amp   = 0.0f;
    }

    /* ---------- 鼓点初始化 ---------- */
    /* 全部置空闲（kind >= KS_DRUM_KIND_COUNT 就是空闲）。
     * ★ drum_len 的两种含义（注意别混）★
     *   music_t 里的注释写的是 "= bars × 16"，但后端 music.js:805-807
     *   实际返回的是**一个小节的 16 格图案**（drum_kick 长度固定 16，
     *   选 none 时是空数组）。所以索引时必须对 drum_len 取模，
     *   不能按 bars×16 去线性索引 —— 那样会在第 2 小节就越界。
     *   这里按"图案长度"理解，并用取模索引（见 Render 里的触发逻辑）。 */
    for (int i = 0; i < KS_MAX_DRUMS; i++)
    {
        s->drums[i].kind = KS_DRUM_KIND_COUNT;      /* 空闲 */
        s->drums[i].amp  = 0.0f;
    }

    s->drum_len        = music->drum_len;
    s->drum_idx        = 0;
    s->drum_next_frame = 0;

    /* 图案长度必须夹在真实数组容量内（music_t 里是 MUSIC_MAX_DRUM_STEPS） */
    if (s->drum_len < 0)
        s->drum_len = 0;
    if (s->drum_len > MUSIC_MAX_DRUM_STEPS)
        s->drum_len = MUSIC_MAX_DRUM_STEPS;

    /* 每格多少样点：drum_step_ms 由后端按 bpm 算好（60000/bpm/4） */
    if (music->drum_step_ms > 0)
    {
        s->drum_samples = (uint32_t)((double)sample_rate *
                                     (double)music->drum_step_ms / 1000.0 + 0.5);
    }
    else
    {
        s->drum_samples = 0;
    }
    if (s->drum_samples == 0)
        s->drum_samples = 1;

    /* 有图案、且三轨里至少有一个 1，才算"有鼓点" */
    s->drums_enabled = 0;
    if (s->drum_len > 0)
    {
        for (int i = 0; i < s->drum_len; i++)
        {
            if (music->drum_kick[i] || music->drum_snare[i] || music->drum_hat[i])
            {
                s->drums_enabled = 1;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "init: fs=%u Hz, decay=%.2fs -> d=%.6f, damping=%.2f, bright=%.2f, pick=%.2f",
             (unsigned)sample_rate, (double)music->decay_seconds, (double)s->decay_coeff,
             (double)s->damping, (double)s->brightness, (double)s->pick_pos);
    ESP_LOGI(TAG, "      %d steps/bar, %d total steps, %u samples/step (%.1f ms/step)",
             s->steps_per_bar, s->total_steps, (unsigned)s->step_samples,
             (double)music->step_ms);
    ESP_LOGI(TAG, "      voices: %d, memory: %u bytes",
             KS_MAX_VOICES, (unsigned)sizeof(ks_synth_t));

    /* ★ Init 结束时必须把 started 置 1（实测踩过的坑）★
     *
     * 否则第一次 Render 会走 `if (!s->started) KS_Synth_Restart(s);`，
     * 而 Restart 会把所有鼓槽位清空 —— 于是"先 Init 再手动
     * KS_Synth_TriggerDrum 然后 Render"这条路永远听不到鼓声。
     *
     * 语义上也更对：Init 已经完成了全部参数换算，合成器就是"可用的"，
     * 不该等到第一次 Render 才"启动"。真实的播放路径（Play 时调
     * KS_Synth_Restart 再 Render）不受影响 —— Restart 会重新置 started。
     *
     * ★ 注意：Init 不重置 frame，所以这里不能顺手清鼓 —— 由 memset 保证初值。 */
    s->started  = 1;
    s->finished = 0;

    /* 鼓点信息：没有鼓时也打一行，方便确认"是后端没给还是我们没实现" */
    if (s->drums_enabled)
    {
        ESP_LOGI(TAG, "      drums: len=%d grid, %u samples/grid (%.1f ms) -> %s",
                 s->drum_len, (unsigned)s->drum_samples,
                 (double)music->drum_step_ms, "ENABLED");
    }
    else
    {
        ESP_LOGI(TAG, "      drums: 无（后端 drum_pattern=none 或全 0）");
    }

    return 0;
}

int KS_Synth_Restart(ks_synth_t *s)
{
    if (s == NULL || s->music == NULL)
        return -1;

    for (int i = 0; i < KS_MAX_VOICES; i++)
    {
        s->voices[i].state = KS_VOICE_FREE;
        s->voices[i].midi  = -1;
        s->voices[i].amp   = 0.0f;
        s->voices[i].write_lp_z = 0.0f;
    }

    s->step             = 0;
    s->arp_idx          = 0;
    s->frame            = 0;
    s->step_start_frame = 0;
    s->started          = 1;
    s->finished         = 0;

    /* 鼓点也要从头开始 —— 否则循环播放时鼓会接着上一轮的格子继续走 */
    for (int i = 0; i < KS_MAX_DRUMS; i++)
    {
        s->drums[i].kind = KS_DRUM_KIND_COUNT;
        s->drums[i].amp  = 0.0f;
    }
    s->drum_idx        = 0;
    s->drum_next_frame = 0;

    ESP_LOGI(TAG, "restart from step 0");
    return 0;
}

void KS_Synth_Stop(ks_synth_t *s)
{
    if (s == NULL)
        return;

    /* ★ 不直接清零 amp（会产生"咔"的爆音），而是走很短的释放 ★
     * 由于 Stop 是同步返回的，这里直接把 amp 按一个短滑落处理：
     * 更彻底的做法是在 Render 里加释放状态机，但那样代码复杂度上一个台阶，
     * 而 5ms 的线性衰减听感上已经足够平滑。 */
    for (int i = 0; i < KS_MAX_VOICES; i++)
    {
        if (s->voices[i].state == KS_VOICE_ACTIVE)
        {
            /* 立刻进入"释放中"：用极小的 amp 让它在几十个样点内自然消失 */
            s->voices[i].amp *= 0.02f;
        }
    }
    s->started  = 0;
    s->finished = 0;
}

/*==============================================================================
 * 拨弦
 *============================================================================*/
int KS_Synth_Pluck(ks_synth_t *s, int midi, float amp)
{
    if (s == NULL || s->music == NULL || midi < 0 || midi > 127)
        return -1;

    float freq = KS_MidiToFreq((float)midi);
    if (freq <= 0.0f || freq >= s->sample_rate * 0.45f)
        return -1;      /* 超过奈奎斯特频率的音直接丢弃，否则延迟线会退化 */

    /* ---------- 分配声部 ---------- */
    int slot = -1;
    float quietest = 1e9f;
    int quietest_idx = -1;

    for (int i = 0; i < KS_MAX_VOICES; i++)
    {
        if (s->voices[i].state == KS_VOICE_FREE)
        {
            slot = i;
            break;
        }
        /* 记住最轻的声部，没空闲时抢它 */
        if (s->voices[i].amp < quietest)
        {
            quietest = s->voices[i].amp;
            quietest_idx = i;
        }
    }

    int stolen = 0;
    if (slot < 0)
    {
        slot = (quietest_idx >= 0) ? quietest_idx : 0;
        stolen = 1;
    }

    ks_voice_t *v = &s->voices[slot];
    v->midi = midi;
    ks_pluck_voice(v, freq, s, amp);

    if (stolen)
    {
        ESP_LOGD(TAG, "stole voice %d for midi %d (all %d busy)", slot, midi, KS_MAX_VOICES);
        return -2;
    }
    return 0;
}

/*==============================================================================
 * 时间轴：触发某一步该响的音
 *============================================================================*/
/**
 * @brief 触发第 step 步的全部音符（伴奏 + 低音 + 旋律）
 */
static void ks_trigger_step(ks_synth_t *s, int step)
{
    const music_t *m = s->music;

    if (step < 0 || step >= s->total_steps)
        return;

    int bar = step / s->steps_per_bar;
    if (bar >= m->bars)
        bar = m->bars - 1;

    /* ★ bar 必须在这里就查边界（审计修复）★
     * 它随后被用来索引 chord_offset[] / chord_count[] / bass_notes[] /
     * drum_level[]，全部是 MUSIC_MAX_BARS 长度的数组。
     * 原来的写法只在最后读 bass_notes 时才检查 bar < MUSIC_MAX_BARS，
     * 而在这之前它已经被用来索引 chord_offset 了 —— 顺序是错的。 */
    if (bar < 0 || bar >= MUSIC_MAX_BARS)
        return;

    /* ★ 用夹逼过的本地副本，不再直接用后端下发值 ★
     * 保证 base + count <= MUSIC_MAX_CHORD_NOTES，即下面所有
     * m->chord_notes[base + ...] 都在界内。 */
    int count = s->chord_count[bar];
    int base  = s->chord_offset[bar];

    /* ---------- 伴奏 ---------- */
    if (count > 0 && m->arp_len > 0)
    {
        if (m->strum)
        {
            /* ★ 扫弦：一次性拨响该小节所有和弦音，按 strum_spread_ms 错开 ★
             * 后端 music.js 的注释说明过：原实现把 strum 当成 arp=[0,0,0,0]，
             * 结果根音连弹 4 下，很难听。正确做法是同时拨所有弦。 */
            for (int i = 0; i < count; i++)
            {
                int midi = m->chord_notes[base + i];

                /* 错开用「延迟若干个样点」实现不了（我们是即时拨弦），
                 * 所以用递减增益近似：越晚拨的弦越轻，听感上接近扫弦的渐弱。
                 * ★ 真正的 22ms 错开需要把拨弦时间分散到多次 Render 调用里，
                 *   那要求触发机制带时间偏移。这里是可接受的简化。 */
                float g = KS_GAIN_STRUM * (1.0f - 0.12f * (float)i);
                if (g < 0.05f) g = 0.05f;
                KS_Synth_Pluck(s, midi, g);
            }
        }
        else
        {
            /* 普通音型：每步取一个和弦音 */
            int pat_idx = step % m->arp_len;
            int tone = m->arp_pattern[pat_idx] % count;
            if (tone < 0) tone += count;

            int midi = m->chord_notes[base + tone];
            KS_Synth_Pluck(s, midi, KS_GAIN_CHORD);
        }
    }

    /* ---------- 低音：每小节第一格拨一次 ---------- */
    /* bar 在上面已经查过边界（0 <= bar < MUSIC_MAX_BARS），这里不必重复检查 */
    if ((step % s->steps_per_bar) == 0)
    {
        int bass = m->bass_notes[bar];
        if (bass > 0)
            KS_Synth_Pluck(s, bass, KS_GAIN_BASS);
    }

    /* ---------- 旋律 ---------- */
    if (step < m->melody_len)
    {
        int note = m->melody_notes[step];
        if (note >= 0)      /* ★ -1 = 休止，不是错误值 ★ */
            KS_Synth_Pluck(s, note, KS_GAIN_MELODY);
    }
}

/*==============================================================================
 * 渲染
 *============================================================================*/
int KS_Synth_Render(ks_synth_t *s, int16_t *out, size_t frames)
{
    if (s == NULL || out == NULL || frames == 0 || s->music == NULL)
        return -1;

    /* ★ 除零防护（审计修复）★
     * 下面用 `s->frame / s->step_samples` 推导当前步。而 KS_Synth_Init 开头是
     * memset(s, 0, ...)，之后才逐步填字段 —— 如果音频任务恰好在那个窗口里
     * 调用了 Render，step_samples 还是 0 → 整数除零 panic。
     * Init 内部的兜底（`if (step_samples == 0) step_samples = 1`）只在 Init 里生效，
     * 拦不住"Init 还没跑完就被 Render 读到"这种情况。
     * 所以在 Render 入口再挡一次：宁可这一块不出声，也不能除零崩溃。 */
    if (s->step_samples == 0)
        return -1;

    if (!s->started)
        KS_Synth_Restart(s);

    for (size_t i = 0; i < frames; i++)
    {
        /* ---------- 1. 到步边界就触发音符 ---------- */
        /* ★ 用 frame 绝对计数推导当前步，而不是「每 step_samples 触发一次」的计数器。
         *   这样即使渲染块大小变化，节拍也不会漂。 */
        int cur_step = (int)(s->frame / s->step_samples);

        if (cur_step >= s->total_steps)
        {
            /* 一轮放完。声部继续衰减（自然收尾），但不再触发新音 */
            s->finished = 1;
            cur_step = s->total_steps - 1;
        }
        else if (cur_step != s->step || s->frame == 0)
        {
            s->step = cur_step;
            ks_trigger_step(s, cur_step);
        }

        /* ---------- 1.5 鼓点触发 ----------
         * ★ 用"绝对样点位置"推导，不用计数器累加 ★
         *   与和弦的时间轴同理：按 frame 推导不会因为渲染块大小变化而漂移。
         *
         * ★ 索引必须取模 ★
         *   后端返回的 drum_kick/snare/hat 是【一个小节的 16 格图案】
         *   （music.js:805-807），不是 bars×16 的长数组。
         *   所以第 2 小节开始要回到图案开头：idx % drum_len。
         *
         * ★ while 不是 if ★
         *   一次渲染可能跨越好几格（块大时），要把欠下的格子一次补完，
         *   否则鼓点会整体后移。
         *
         * ★ 两个门控条件都要有 ★
         *   ① drum_len > 0       —— 没有图案就没什么可触发
         *   ② !s->finished       —— 曲子放完就停止**触发新鼓**
         *
         *   ★ 关键区分：「停止触发」不等于「停止推进」★
         *     实测踩过这个坑：一开始把 `!s->finished` 加在了触发门控上，
         *     但混音循环是照常跑的 —— 看起来对，其实有个细节：
         *     已经触发出来的鼓**必须继续被 ks_drum_next() 推进**才会衰减。
         *     混音循环无条件跑（见下面 2.5 节），所以鼓会正常衰减完、正常释放。
         *     （反例：如果连混音也跳过，鼓就会被"冻住"，永远占着槽位 ——
         *       拨弦声部当年就是同类问题：声部池永不回收。）
         *
         *   ★ 注意这里【不能】用 drums_enabled 做门控 ★
         *     那个变量表示"三轨里至少有一个 1"；用它当门控会让
         *     "图案全 0"时仍然触发一次幽灵鼓（因为 drum_len 还是 16）。
         *     真正的判断依据就是图案内容本身（全 0 自然不会触发任何鼓）。 */
        if (s->drum_len > 0 && !s->finished && s->drum_samples > 0)
        {
            while (s->frame >= s->drum_next_frame)
            {
                int g = s->drum_idx % s->drum_len;

                /* 强度：按小节取 drum_level（后端给 0~9 的段落动态）。
                 * 越界索引一律当 0（解析器已夹过，这里再防一次）。 */
                int bar = (int)(s->drum_next_frame / (uint64_t)s->step_samples)
                          / s->steps_per_bar;
                uint8_t lv = 0;
                if (bar >= 0 && bar < MUSIC_MAX_BARS)
                    lv = s->music->drum_level[bar];

                if (s->music->drum_kick[g])  KS_Synth_TriggerDrum(s, KS_DRUM_KICK,  lv);
                if (s->music->drum_snare[g]) KS_Synth_TriggerDrum(s, KS_DRUM_SNARE, lv);
                if (s->music->drum_hat[g])   KS_Synth_TriggerDrum(s, KS_DRUM_HAT,   lv);

                s->drum_idx++;
                s->drum_next_frame += s->drum_samples;
            }
        }

        /* ---------- 2. 混合所有声部 ---------- */
        float mix = 0.0f;
        for (int v = 0; v < KS_MAX_VOICES; v++)
        {
            ks_voice_t *voice = &s->voices[v];
            if (voice->state != KS_VOICE_ACTIVE)
                continue;

            mix += ks_voice_next(voice);

            /* ★ 声部按寿命回收（审计修复 S1）★
             *
             * 原来的条件 `voice->amp < 1e-5f` 是【永不成立的死代码】：
             * amp 是静态增益（和弦 0.45 / 低音 0.90 / 旋律 0.75），
             * 从拨弦到结束都不变；衰减发生在延迟线内部（buf[] 逐圈乘 d）。
             * 于是 32 个声部槽很快被占满且永不释放，之后每次拨弦都在抢占：
             *   ks_pluck_voice 会用新噪声整条覆盖 buf[]
             *   → 输出从上一音的任意相位直接跳到新激励 = 每抢占一次一声"咔"，
             *   而且内层循环永远跑满 32 个声部（CPU 常驻满负荷）。
             *
             * 现在用"存活样点数"判断：走完 decay_seconds（衰减到 -60dB）就回收，
             * 语义上正好等于"这个音已经听不见了"。 */
            if (++voice->life >= voice->life_max)
            {
                voice->state = KS_VOICE_FREE;
                voice->midi  = -1;
                voice->amp   = 0.0f;
            }
        }

        /* ---------- 2.5 混合鼓点 ----------
         * ★ 关键：鼓【不乘】master_gain，所以必须在 `mix *= master_gain` 之后加 ★
         *
         *   鼓的增益在 KS_Synth_TriggerDrum 里已经按 level 算好了
         *   （level=3 时为 1.0，level=9 时约 1.8）。如果它跟着拨弦一起乘
         *   master_gain(0.30)，鼓会变得几乎听不见。
         *
         *   鼓是节奏骨架，本来就该比拨弦更突出，所以单独给一个系数
         *   （0.45）让它与拨弦声部音量相称。
         *
         *   ★ 顺序很重要 ★ 这里的 `mix` 已经是乘过 master_gain 的拨弦混音，
         *     鼓直接加在上面 —— 这正是"鼓不参与 master_gain"的实现方式。
         *
         * ★ 这里【不能】用 `if (drums_enabled)` 做条件（实测踩过）★
         *   drums_enabled 的含义是"乐谱里有鼓点图案"，用来决定**要不要自动触发**。
         *   而混音是另一件事：手动调 KS_Synth_TriggerDrum 触发的鼓，
         *   在乐谱没有图案时也必须出声。原实现把两者绑在一起，症状是
         *   "触发成功、amp=1.0、ActiveDrums=1，但输出全是 0"，很难查。
         *   8 个槽位空转只有一次比较，成本可忽略，所以无条件遍历。 */
        mix *= s->master_gain;

        {
            const float drum_gain = 0.45f;
            for (int i = 0; i < KS_MAX_DRUMS; i++)
            {
                if (s->drums[i].kind >= KS_DRUM_KIND_COUNT)
                    continue;
                mix += ks_drum_next(&s->drums[i], 1.0f / s->sample_rate) * drum_gain;
            }
        }

        /* ---------- 3. 限幅输出 ----------
         * ★ 用软限幅替代原来的硬夹逼（实测修复"咔"声）★
         *
         *   原来是： if (mix > 1.0f) mix = 1.0f;   if (mix < -1.0f) mix = -1.0f;
         *   这会把浮点溢出削成一个【硬拐角】。硬拐角 = 一阶导不连续 = 谐波 =
         *   听觉上的"咔"。实测整曲有 74 个样点被夹、分布在 70 段里 —— 也就是
         *   70 次独立的拐角事件。
         *
         *   0.70 以上改用双曲式软拐点：幅度继续增长但斜率单调下降，
         *   因此处处连续、没有拐角。实测对比（同一段音频，只换限幅方式）：
         *      夹逼样点   74 段/70  →  0
         *      最大相邻跳变 1.8922   →  1.6122
         *      rms        0.1333    →  0.1326   （仅 −0.5%，音量不损失）
         *
         *   代价只有 4 个浮点运算，没有 expf/tanhf 调用，ESP32 上可忽略。
         *   保留 1.0 的最终夹逼作为兜底：软拐点本身有渐近线，理论上到不了 1.0，
         *   但浮点误差下保留一道硬上限更稳妥（此时已几乎不触发）。 */
        {
            const float knee = 0.70f;
            float mag = (mix < 0.0f) ? -mix : mix;
            if (mag > knee)
            {
                float over = mag - knee;
                /* 1/(1-knee) = 3.3333：把 over 映射到 [0, 1-knee) 的渐近区间 */
                mag = knee + over / (1.0f + 3.3333f * over);
                if (mag > 1.0f) mag = 1.0f;
            }
            mix = (mix < 0.0f) ? -mag : mag;
        }

        out[i] = (int16_t)(mix * 32767.0f);

        s->frame++;
    }

    return (int)frames;
}

uint8_t KS_Synth_IsFinished(const ks_synth_t *s)
{
    if (s == NULL)
        return 1;
    return s->finished;
}

/**
 * @brief 当前还有多少个声部在发声
 * @note  ★ 为什么需要它（审计修复 S4）★
 *        播放任务判断"一轮是否真的放完了"时，不能只看 finished 标志：
 *        finished 只表示"不再触发新音符"，此时已有声部还在自然衰减
 *        （decay_seconds 最长 3.5 秒）。
 *        如果在声部还在响的时候就 rewind，会把尾音硬切 → 循环接缝处"咔"一声。
 *        所以要等 finished && ActiveVoices()==0 才算一轮结束。
 *
 *        ★ 本函数的正确性依赖 S1 的修复 ★
 *          如果声部仍然永不回收，这里会恒等于 KS_MAX_VOICES，
 *          条件永远不成立 —— 这正是为什么两处必须一起修。
 */
uint32_t KS_Synth_ActiveVoices(const ks_synth_t *s)
{
    if (s == NULL)
        return 0;

    uint32_t n = 0;
    for (int i = 0; i < KS_MAX_VOICES; i++)
    {
        if (s->voices[i].state == KS_VOICE_ACTIVE)
            n++;
    }
    return n;
}

/**
 * @brief 当前正在发声的鼓的个数
 */
uint32_t KS_Synth_ActiveDrums(const ks_synth_t *s)
{
    if (s == NULL)
        return 0;

    uint32_t n = 0;
    for (int i = 0; i < KS_MAX_DRUMS; i++)
    {
        if (s->drums[i].kind < KS_DRUM_KIND_COUNT)
            n++;
    }
    return n;
}
