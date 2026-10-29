/**
 * @file drum_clip_check.c
 * @brief 鼓点混音质量诊断：量化"咔"声到底是软件削顶还是别的
 *
 * @note  ★ 为什么需要它 ★
 *        用户实测（2026-10）：「扬声器在播放类似敲打声音沙沙的镲片的那种声音时
 *        会咔一声」。听感上"咔"有两个完全不同的来源，处理方式也完全不同：
 *
 *          ① 软件削顶（clipping）
 *             mix 超过 ±1.0 被 ks_synth.c 的硬限幅切平 → 波形出现方波台阶
 *             → 听感"噼/咔"。修法：加 headroom / 软限幅 / 降低鼓增益。
 *
 *          ② DMA 欠载（underrun）
 *             音频任务没及时补数据，DMA 缓冲放空 → 波形突然中断
 *             → 听感同样是"咔"。修法：改 I2S 写入策略，与合成器无关。
 *
 *        两者听感几乎一样，但本案的波形证据可以一刀切开：
 *        削顶必然表现为【连续多个样点 = ±32767】，且只在鼓（尤其 hat）触发时出现；
 *        欠载在离线渲染里根本不存在（离线渲染是在 PC 上一次性算完的）。
 *        所以：如果离线渲染里就抓到大量削顶，那软件侧确实是原因之一。
 *
 *        本文件【不属于固件】，不会被 ESP-IDF 编译（放在 tools/ 目录下）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ---- 用桩头文件替代 esp_log.h / esp_random.h（见 tools/stub/）---- */
#include "esp_log.h"
#include "esp_random.h"

esp_log_level_t g_log_level = ESP_LOG_ERROR;   /* 诊断时只关心错误 */

static uint32_t g_rng = 0x12345678u;

uint32_t esp_random(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
void esp_random_seed(uint32_t seed) { g_rng = seed ? seed : 1u; }

#include "ks_synth.h"

/*==============================================================================
 * 增益参数（可按取值扫，用来确定合适的 headroom）
 * @note  G_FIX 是本工具对合成器的【修正系数】。为了让扫描不改动生产源码，
 *        run_drum_clip_check.ps1 会先把 ks_synth.c 复制成临时文件并在
 *        KS_Synth_TriggerDrum 的 switch 之后注入：
 *            d->amp *= G_FIX[(int)kind];
 *        这样扫描的是"整体缩放"，音色相对关系不变。
 *
 * ★ 必须是【非 static】的全局变量（实测踩过）★
 *   注入代码位于**另一个翻译单元**（ks_synth_probe.c）。如果这里写 static，
 *   两个文件会各自拥有一个同名但互不相干的 G_FIX：
 *     - 本文件解析 -g 参数、写的是自己的那一份
 *     - 探针读的是它自己那份，永远是初始值 1.0
 *   症状是【扫描/r -g 完全没有效果】，而且不报任何错 —— 参数打印出来是对的，
 *   波形却一模一样。这个 bug 浪费了一整轮排查。
 *   现在：本文件用【非 static 定义】，探针只写 extern 声明，两边共享同一个数组。
 *============================================================================*/
float G_FIX[3]           = { 1.00f, 1.00f, 1.00f };   /* kick, snare, hat */
static float G_DRUM      = 0.45f;                      /* 整体鼓增益 */
static float G_MASTER    = 0.30f;                      /* 拨弦 master_gain */

/*==============================================================================
 * 削顶归因：每个音色在【最终混音里】贡献的最大幅度
 *
 * @note  ★ 为什么需要它 ★
 *        整曲扫描时，snare 修正系数从 1.0 一路降到 0.5，削顶样点数始终 ~70、
 *        peak 始终 1.0000 —— 这本身就证明【削顶不是 snare 造成的】。
 *        与其继续猜是 kick 还是 hat，不如让混音循环把每个音色各自的贡献记下来。
 *
 * 定义在探针（ks_synth_probe.c）里，因为它必须插进 ks_synth.c 的混音循环中；
 * 本文件只做 extern 声明并读取结果。
 *============================================================================*/
extern float g_probe_trk[3];

/** 1 = 用软限幅替代硬限幅（由探针读取，A/B 实验用） */
extern int g_probe_soft;

/** snare 一阶低通级联系数；0 = 关闭。由探针的 snare 分支读取 */
extern float g_probe_snarelp;
extern float g_probe_snz1, g_probe_snz2;

/*==============================================================================
 * 配置
 *============================================================================*/
#define FS           16000          /* 与固件一致 */
#define BARS         4
#define STEPS_PER_BAR 8

/*==============================================================================
 * 统计结构：比 ks_test.c 的 analyze() 多几个"咔声指纹"指标
 *============================================================================*/
typedef struct {
    size_t n;
    double rms;
    double peak;
    double dc;              /* 直流偏置（导致喇叭"啵"声/浪费动态余量） */
    size_t clip_pos;        /* 削到 +32767 的样点数 */
    size_t clip_neg;        /* 削到 -32768 的样点数 */
    size_t clip_runs;       /* 连续削顶"段"数（真削顶的特征） */
    size_t max_run;         /* 最长连续削顶长度（样点） */
    double max_delta;       /* 相邻样点最大跳变（瞬态/不连续指纹） */
    size_t max_delta_at;    /* ★ 最大跳变发生的样点下标 —— 定位不连续位置 */
    size_t big_jumps;       /* 跳变 > 0.25 全量程的次数 */
    size_t huge_jumps;      /* ★ 跳变 > 0.6 全量程的次数（必定可闻的"咔"） */
} wstat_t;

static wstat_t wanalyze(const int16_t *pcm, size_t n)
{
    wstat_t s;
    memset(&s, 0, sizeof(s));
    if (n == 0) return s;

    double sum2 = 0.0, sum = 0.0, peak = 0.0;
    size_t run = 0;

    for (size_t i = 0; i < n; i++)
    {
        double v = (double)pcm[i] / 32768.0;
        sum  += v;
        sum2 += v * v;
        if (fabs(v) > peak) peak = fabs(v);

        if (pcm[i] >= 32767)       s.clip_pos++;
        else if (pcm[i] <= -32768) s.clip_neg++;

        int at_full = (pcm[i] >= 32767 || pcm[i] <= -32768);
        if (at_full) {
            run++;
            if (run == 1) s.clip_runs++;
            if (run > s.max_run) s.max_run = run;
        } else {
            run = 0;
        }

        if (i > 0) {
            double d = fabs(v - (double)pcm[i - 1] / 32768.0);
            if (d > s.max_delta) { s.max_delta = d; s.max_delta_at = i; }
            if (d > 0.25) s.big_jumps++;
            if (d > 0.60) s.huge_jumps++;
        }
    }

    s.n    = n;
    s.rms  = sqrt(sum2 / (double)n);
    s.dc   = sum / (double)n;
    s.peak = peak;
    return s;
}

static void wprint(const char *tag, const wstat_t *s, int fs)
{
    double dur = (double)s->n / (double)fs;
    double clips = (double)(s->clip_pos + s->clip_neg);
    printf("  %-22s 时长=%5.2fs  peak=%.4f  rms=%.4f  dc=%+.5f\n",
           tag, dur, s->peak, s->rms, s->dc);
    printf("  %-22s 削顶: 共 %zu 样点 (%.3f%%)  +%zu/-%zu  %zu 段  最长连续 %zu 样点\n",
           "", s->clip_pos + s->clip_neg,
           (s->n ? 100.0 * clips / (double)s->n : 0.0),
           s->clip_pos, s->clip_neg, s->clip_runs, s->max_run);
    printf("  %-22s 瞬态: 最大相邻跳变=%.4f  >0.25 的跳变 %zu 次\n",
           "", s->max_delta, s->big_jumps);
}

/*==============================================================================
 * 构造乐谱
 *============================================================================*/
static music_t *make_music(int with_drums)
{
    music_t *m = (music_t *)calloc(1, sizeof(music_t));
    if (m == NULL) return NULL;

    m->bpm              = 80;
    m->bars             = BARS;
    m->beats_per_bar    = 4;
    m->steps_per_beat   = 2;
    m->step_ms          = 375;
    m->instrument_index = 0;
    m->decay_seconds    = 2.0f;
    m->brightness       = 0.6f;
    m->damping          = 0.38f;
    m->pick_position    = 0.40f;
    m->strum            = 0;
    m->arp_len          = 1;

    /* 和弦：每小节 3 个音（A3 C4 E4） */
    int idx = 0;
    for (int b = 0; b < BARS; b++) {
        m->chord_note_counts[b] = 3;
        for (int k = 0; k < 3 && idx < MUSIC_MAX_CHORD_NOTES; k++) {
            static const int16_t chord[3] = { 57, 60, 64 };
            m->chord_notes[idx++] = chord[k];
        }
    }
    m->chord_notes_len = (int16_t)idx;

    m->melody_len = (int16_t)(BARS * STEPS_PER_BAR);
    for (int i = 0; i < m->melody_len && i < MUSIC_MAX_MELODY_STEPS; i++)
        m->melody_notes[i] = 69;

    for (int b = 0; b < BARS; b++)
        m->bass_notes[b] = 45;

    if (with_drums) {
        /* 后端 DRUM_PATTERNS.pop（music.js:310-314），密集 8 分音符 hat */
        static const uint8_t k[16] = {1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,0};
        static const uint8_t s[16] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,1};
        static const uint8_t h[16] = {1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,1};
        for (int i = 0; i < 16; i++) {
            m->drum_kick[i]  = k[i];
            m->drum_snare[i] = s[i];
            m->drum_hat[i]   = h[i];
        }
        m->drum_len     = 16;
        m->drum_step_ms = 125;
        for (int b = 0; b < MUSIC_MAX_BARS; b++)
            m->drum_level[b] = 3;
    } else {
        m->drum_len = 0;
    }
    return m;
}

/** 渲染整曲到内存 */
static int16_t *render_all(music_t *m, size_t *out_n)
{
    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
    if (s == NULL) return NULL;

    int r = KS_Synth_Init(s, m, FS);
    if (r != 0) { free(s); return NULL; }

    size_t cap = (size_t)FS * 90;
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * cap);
    if (pcm == NULL) { free(s); return NULL; }

    size_t done = 0;
    while (done < cap) {
        size_t want = cap - done;
        if (want > 1024) want = 1024;
        int n = KS_Synth_Render(s, pcm + done, want);
        if (n <= 0) break;
        done += (size_t)n;

        if (KS_Synth_IsFinished(s) &&
            KS_Synth_ActiveVoices(s) == 0 &&
            KS_Synth_ActiveDrums(s) == 0)
            break;
    }
    free(s);
    *out_n = done;
    return pcm;
}

/** 只渲染单个鼓（不触发任何拨弦/旋律） */
static int16_t *render_one_drum(KS_DrumKind_t kind, uint8_t level, size_t *out_n)
{
    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
    music_t    *m = make_music(0);
    if (s == NULL || m == NULL) { free(s); free(m); return NULL; }

    if (KS_Synth_Init(s, m, FS) != 0) { free(s); free(m); return NULL; }

    /* 手动只触发一个鼓（不注册乐谱图案，所以不会被自动触发） */
    KS_Synth_TriggerDrum(s, kind, level);

    size_t cap = (size_t)(FS * 0.6);
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * cap);
    if (pcm == NULL) { free(s); free(m); return NULL; }

    size_t done = 0;
    while (done < cap) {
        size_t want = cap - done;
        if (want > 512) want = 512;
        int n = KS_Synth_Render(s, pcm + done, want);
        if (n <= 0) break;
        done += (size_t)n;
    }
    free(s); free(m);
    *out_n = done;
    return pcm;
}

/**
 * @brief 抖出前 limit 个"大跳变"的位置与上下文
 * @note  ★ 为什么需要它 ★
 *        "咔"声的波形指纹就是【相邻样点的大跳变】。但只看"最大跳变"数值
 *        无法区分两种情况：
 *          · 噪声（hat/snare 的白噪声本来就是逐样点随机，跳变大是正常的）
 *          · 真正的不连续（波形被硬切：槽位被抢占、限幅、包络突变）
 *        所以打印每个大跳变前后的样点值：
 *          · 若是噪声 → 前后值杂乱无规律，且跳变均匀分布
 *          · 若是不连续 → 前一个值接近某个平滑波形，后一个值突然跳到远处
 */
static void dump_jumps(const char *tag, const int16_t *pcm, size_t n, double thr, int limit)
{
    printf("  [%s] 跳变 > %.2f 的前 %d 处：\n", tag, thr, limit);
    int shown = 0;
    for (size_t i = 1; i < n && shown < limit; i++)
    {
        double a = (double)pcm[i - 1] / 32768.0;
        double b = (double)pcm[i]     / 32768.0;
        if (fabs(b - a) > thr)
        {
            printf("      i=%-6zu  %.4f -> %.4f   (跳 %.4f)   上下文",
                   i, a, b, b - a);
            for (size_t k = (i >= 3 ? i - 3 : 0); k < i + 3 && k < n; k++)
                printf(" %+.3f", (double)pcm[k] / 32768.0);
            printf("\n");
            shown++;
        }
    }
    if (shown == 0) printf("      （无）\n");
}

/*==============================================================================
 * 主程序
 *============================================================================*/
/*==============================================================================
 * 扫描模式：找 snare 需要多少 headroom 才不削顶
 *
 * @note  ★ 为什么扫描放在 C 里而不是 PowerShell 里 ★
 *        在 PS 里解析 C 程序的中文输出会被 ANSI 解码毁掉（实测踩过：
 *        "削顶" 变成 "鍓婇《"，然后 PS 解析器直接报 Unexpected token）。
 *        更稳的做法是【编译一次、在 C 内部循环调用】，完全不经过文本解析。
 *============================================================================*/
static void run_sweep(void)
{
    printf("\n=== 扫描：snare 修正系数 → level=9 的削顶情况 ===\n");
    printf("  （kick/hat 保持 1.0，drum_gain=0.45；后端 verse/main/chorus 都走 level=9 这条路）\n");
    printf("  maxK/maxS/maxH = 该音色在【最终混音里】贡献的最大幅度（1.0 = 满量程）\n\n");
    printf("  %-10s %-10s %-10s %-10s %-8s %-8s %-8s\n",
           "snareFix", "peak", "clipSamples", "maxRun", "maxK", "maxS", "maxH");
    printf("  ---------- ---------- ---------- ---------- -------- -------- --------\n");

    static const float fix[] = { 1.00f, 0.95f, 0.92f, 0.90f, 0.85f, 0.80f,
                                 0.75f, 0.70f, 0.65f, 0.60f, 0.55f, 0.50f };
    for (size_t k = 0; k < sizeof(fix) / sizeof(fix[0]); k++)
    {
        G_FIX[0] = 1.00f;
        G_FIX[1] = fix[k];
        G_FIX[2] = 1.00f;

        music_t *m = make_music(1);
        if (m == NULL) continue;
        for (int b = 0; b < MUSIC_MAX_BARS; b++) m->drum_level[b] = 9;

        g_probe_trk[0] = g_probe_trk[1] = g_probe_trk[2] = 0.0f;

        size_t n = 0;
        int16_t *p = render_all(m, &n);
        free(m);
        if (p == NULL) continue;

        wstat_t s = wanalyze(p, n);
        printf("  %-10.3f %-10.4f %-10zu %-10zu %-8.4f %-8.4f %-8.4f %s\n",
               (double)fix[k], s.peak,
               s.clip_pos + s.clip_neg, s.max_run,
               (double)g_probe_trk[0], (double)g_probe_trk[1], (double)g_probe_trk[2],
               (s.clip_pos + s.clip_neg) == 0 ? "<== clean" : "");
        free(p);
    }
}

/**
 * @brief 用固定随机种子渲染单个鼓，并报告峰值/削顶
 * @note  ★ 为什么要固定种子 ★
 *        snare/hat 用白噪声。不固定种子时两次运行的波形完全不同，
 *        数值无法比较 —— 这正是第一版扫描得不出结论的原因
 *        （削顶数在 66~102 之间随机跳动，看不出单调趋势）。
 */
static void probe_drum(const char *tag, KS_DrumKind_t kind, uint8_t level)
{
    esp_random_seed(0xC0FFEEu);
    size_t n = 0;
    int16_t *p = render_one_drum(kind, level, &n);
    if (p == NULL) { printf("  %s: alloc fail\n", tag); return; }
    wstat_t s = wanalyze(p, n);
    printf("  %-16s peak=%.4f rms=%.4f dc=%+.5f  削顶=%zu 最长%zu  最大跳变=%.4f\n",
           tag, s.peak, s.rms, s.dc, s.clip_pos + s.clip_neg, s.max_run, s.max_delta);
    free(p);
}

/**
 * @brief 逐个音色隔离测试：谁才是削顶的真凶
 */
static void run_isolate(void)
{
    printf("\n=== 隔离测试：逐个音色单独渲染，看谁削顶（随机种子固定）===\n");

    struct { const char *nm; KS_DrumKind_t k; } names[] = {
        { "kick",  KS_DRUM_KICK  },
        { "snare", KS_DRUM_SNARE },
        { "hat",   KS_DRUM_HAT   },
    };

    for (int pass = 0; pass < 2; pass++) {
        printf("\n  --- %s ---\n", pass == 0 ? "修正系数全 1.0（当前固件）"
                                             : "各音色修正系数 0.5");
        for (size_t i = 0; i < 3; i++) {
            G_FIX[0] = G_FIX[1] = G_FIX[2] = (pass == 0) ? 1.0f : 0.5f;
            for (int lv = 3; lv <= 9; lv += 3) {
                char tag[48];
                snprintf(tag, sizeof(tag), "%s lv=%d", names[i].nm, lv);
                probe_drum(tag, names[i].k, (uint8_t)lv);
            }
        }
    }
    G_FIX[0] = G_FIX[1] = G_FIX[2] = 1.0f;
}

/**
 * @brief 打印单个鼓在最大跳变处的原始样点，肉眼确认是"波形不连续"还是"正常瞬态"
 *
 * @note  ★ 这是判断"咔"声的关键工具 ★
 *        统计量只能告诉你"有大跳变"，但分不清两种情况：
 *          (a) 正常的鼓击打瞬态 —— 能量从 0 快速上升，相邻差本来就大
 *          (b) 代码 bug 导致的不连续 —— 波形中间突然出现台阶/断点
 *        把跳变点前后 16 个样点打出来，一眼就能分辨。
 *        判定标准：看跳变前后的**局部幅度**。正常瞬态是从静音爬升，
 *        幅度单调增长；bug 不连续是在**已有大幅波形中间**突然跳。
 */
static void dump_jump(const char *tag, KS_DrumKind_t kind, uint8_t level)
{
    esp_random_seed(0xC0FFEEu);
    size_t n = 0;
    int16_t *p = render_one_drum(kind, level, &n);
    if (p == NULL) { printf("  %s: alloc fail\n", tag); return; }

    /* 找最大跳变位置 */
    size_t at = 1; double best = 0.0;
    for (size_t i = 1; i < n; i++) {
        double d = fabs((double)p[i] - (double)p[i-1]);
        if (d > best) { best = d; at = i; }
    }

    printf("\n  === %s (n=%zu) 最大跳变 %.0f (int16) 发生在样点 %zu (t=%.2fms) ===\n",
           tag, n, best, at, 1000.0 * (double)at / (double)FS);

    size_t lo = (at > 16) ? at - 16 : 0;
    size_t hi = (at + 16 < n) ? at + 16 : n;
    printf("    idx      值      |diff|    幅度条\n");
    for (size_t i = lo; i < hi; i++) {
        int diff = (i > 0) ? (int)p[i] - (int)p[i-1] : 0;
        int bars = (int)(fabs((double)p[i]) / 32768.0 * 30.0);
        char bar[40];
        int k = 0;
        for (int b = 0; b < bars && k < 35; b++) bar[k++] = '#';
        bar[k] = '\0';
        printf("    %-8zu %+7d %+8d   %s%s\n",
               i, (int)p[i], diff, bar, (i == at) ? "   <== 最大跳变" : "");
    }
    free(p);
}

/**
 * @brief 转储所有鼓在最大跳变处的波形
 */
static void run_dump(void)
{
    printf("\n=== 波形转储：最大跳变处的原始样点 ===\n");
    dump_jump("snare lv=9", KS_DRUM_SNARE, 9);
    dump_jump("snare lv=3", KS_DRUM_SNARE, 3);
    dump_jump("hat   lv=9", KS_DRUM_HAT,   9);
    dump_jump("kick  lv=9", KS_DRUM_KICK,  9);
}

/*==============================================================================
 * A/B 对比：硬限幅 vs 软限幅（同一段音频，只换限幅方式）
 *
 * @note  ★ 为什么这是决定性实验 ★
 *        硬限幅 `if (mix>1.0f) mix=1.0f;` 把浮点溢出削成一个【硬拐角】。
 *        硬拐角 = 波形的一阶导不连续 = 各次谐波 = 听觉上的"咔"。
 *        软限幅把同一个溢出压成平滑曲线，理论上没有拐角。
 *        两者渲染的是【完全相同的浮点混音】，只差最后一步，
 *        所以任何指标差异都只可能来自限幅方式 —— 这是干净的对照实验。
 *
 * 判定标准：
 *   · maxDelta 明显下降 → 软限幅确实消除了波形拐角
 *   · rms 基本不变      → 音量没有损失（软限幅不是简单降音量）
 *============================================================================*/
static void run_ab(void)
{
    printf("\n=== A/B：硬限幅 vs 软限幅（同源音频）===\n");

    struct { const char *nm; int soft; } modes[] = {
        { "硬限幅 if>1 clamp", 0 },
        { "软限幅 0.70 拐点",  1 },
    };

    for (int i = 0; i < 2; i++) {
        g_probe_soft = modes[i].soft;

        music_t *m = make_music(1);
        if (m == NULL) continue;
        for (int b = 0; b < MUSIC_MAX_BARS; b++) m->drum_level[b] = 9;

        size_t n = 0;
        int16_t *p = render_all(m, &n);
        free(m);
        if (p == NULL) continue;

        wstat_t s = wanalyze(p, n);
        printf("\n  --- %s ---\n", modes[i].nm);
        wprint("mix", &s, FS);
        free(p);
    }

    g_probe_soft = 0;
    printf("\n  说明：peak 两者都会到 1.0（都受满量程约束），\n");
    printf("        所以要看【最大跳变】和【>0.60 的跳变次数】：\n");
    printf("        它们衡量的是波形拐角，也就是\"咔\"的物理来源。\n");
}

/*==============================================================================
 * 扫描 snare 低通：用带限换取"不那么像咔"
 *
 * @note  ★ 为什么这是正解而不是降音量 ★
 *        snare 在 fs=16k、1300Hz 一阶高通下的 Nyquist 响应是 0.914，
 *        也就是说它的能量几乎全在 8kHz。把整体音量降下来，
 *        它还是 8kHz 的"嘶"，只是变轻 —— 听感依旧是"咔"而不是"军鼓"。
 *        只有【低通】能真正削掉 8kHz 能量，把它变成鼓。
 *
 * 表格同时给出 rms（不能掉太多）与 maxDelta/%jump（越小越不像咔），
 * 这样才能判断"换音色"是否值得。
 *============================================================================*/
static void run_snarespec(void)
{
    printf("\n=== 扫描 snare 低通（级联一阶）===Δ 指标越小越不像\"咔\" ===\n");
    printf("  （drum_gain=0.45；用 level=3 密集图案，和测试套件一致）\n\n");
    printf("  %-14s %-8s %-10s %-10s %-10s\n",
           "LP系数", "等效fc", "peak", "rms", "最大跳变");
    printf("  -------------- -------- ---------- ---------- ----------\n");

    static const float lp[] = { 0.0f, 0.85f, 0.75f, 0.65f, 0.50f, 0.35f };
    for (size_t k = 0; k < sizeof(lp) / sizeof(lp[0]); k++)
    {
        g_probe_snarelp = lp[k];

        music_t *m = make_music(1);
        if (m == NULL) continue;
        for (int b = 0; b < MUSIC_MAX_BARS; b++) m->drum_level[b] = 3;

        g_probe_snz1 = g_probe_snz2 = 0.0f;
        size_t n = 0;
        int16_t *p = render_all(m, &n);
        free(m);
        if (p == NULL) continue;

        wstat_t s = wanalyze(p, n);
        /* 等效 -3dB 截止：一阶 y=a*y+(1-a)*x 的 a 对应 fc = -ln(a)*fs/2pi */
        double fc = (lp[k] > 0.0f) ? (-log((double)lp[k]) * FS / (2.0 * M_PI)) : 0.0;
        char fcbuf[16];
        if (lp[k] > 0.0f) snprintf(fcbuf, sizeof(fcbuf), "%.0fHz", fc);
        else              snprintf(fcbuf, sizeof(fcbuf), "off");

        printf("  %-14.2f %-8s %-10.4f %-10.4f %-10.4f\n",
               (double)lp[k], fcbuf, s.peak, s.rms, s.max_delta);
        free(p);
    }
    g_probe_snarelp = 0.0f;
}

int main(int argc, char **argv)
{
    /* 可选：-g kickFix,snareFix,hatFix,drumGain,masterGain */
    if (argc >= 3 && strcmp(argv[1], "-g") == 0) {
        float a=1,b=1,c=1,d=G_DRUM,e=G_MASTER;
        if (sscanf(argv[2], "%f,%f,%f,%f,%f", &a,&b,&c,&d,&e) == 5) {
            G_FIX[0]=a; G_FIX[1]=b; G_FIX[2]=c; G_DRUM=d; G_MASTER=e;
        }
    }
    printf("============================================================\n");
    printf(" 鼓点混音质量诊断 — 量化\"咔\"声来源\n");
    printf(" 采样率 %d Hz；drum_gain=%.3f  master_gain=%.3f\n", FS, (double)G_DRUM, (double)G_MASTER);
    printf(" 修正系数： kick=%.3f  snare=%.3f  hat=%.3f\n",
           (double)G_FIX[0], (double)G_FIX[1], (double)G_FIX[2]);
    printf("============================================================\n");

    if (argc >= 2 && strcmp(argv[1], "-snare") == 0) {
        run_snarespec();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "-ab") == 0) {
        run_ab();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "-dump") == 0) {
        run_dump();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "-isolate") == 0) {
        run_isolate();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "-sweep") == 0) {
        run_sweep();
        return 0;
    }

    /* ---------- 1. 单个鼓的峰值（看有没有先天的越界） ---------- */
    printf("\n=== 1. 单个鼓单独渲染（孤立看每种音色的峰值/直流/瞬态）===\n");
    {
        static const char *nm[3] = { "kick", "snare", "hat" };
        for (int k = 0; k < 3; k++) {
            for (int lv = 3; lv <= 9; lv += 3) {
                size_t n = 0;
                int16_t *p = render_one_drum((KS_DrumKind_t)k, (uint8_t)lv, &n);
                if (p == NULL) { printf("  alloc fail\n"); continue; }
                char tag[64];
                snprintf(tag, sizeof(tag), "%s level=%d", nm[k], lv);
                wstat_t st = wanalyze(p, n);
                /* 单鼓只打印一行紧凑信息 */
                printf("  %-14s peak=%.4f rms=%.4f dc=%+.5f  削顶=%zu 段/最长%zu  最大跳变=%.4f\n",
                       tag, st.peak, st.rms, st.dc,
                       st.clip_runs, st.max_run, st.max_delta);
                free(p);
            }
        }
    }

    /* ---------- 2. 整曲：有鼓 vs 无鼓 ---------- */
    printf("\n=== 2. 整曲渲染对比：无鼓 vs 有鼓（pop 图案，level=3）===\n");
    size_t n_nodrum = 0, n_drum = 0;
    int16_t *pcm_nodrum = NULL, *pcm_drum = NULL;
    {
        music_t *m1 = make_music(0);
        music_t *m2 = make_music(1);
        pcm_nodrum = render_all(m1, &n_nodrum);
        pcm_drum   = render_all(m2, &n_drum);
        free(m1); free(m2);

        if (pcm_nodrum && pcm_drum) {
            wstat_t a = wanalyze(pcm_nodrum, n_nodrum);
            wstat_t b = wanalyze(pcm_drum, n_drum);
            printf("\n  --- 无鼓 ---\n"); wprint("no-drums", &a, FS);
            printf("\n  --- 有鼓 ---\n"); wprint("with-drums", &b, FS);

            printf("\n  ★ 结论线索：\n");
            printf("     有鼓时削顶从 %zu 样点 变成 %zu 样点",
                   a.clip_pos + a.clip_neg, b.clip_pos + b.clip_neg);
            if (b.clip_pos + b.clip_neg > a.clip_pos + a.clip_neg)
                printf("  → 鼓是削顶的来源\n");
            else
                printf("  → 鼓没有引入额外削顶\n");
        }
    }

    /* ---------- 3. 满强度鼓（level=9）—— 后端副歌段会这样 ---------- */
    printf("\n=== 3. 满强度（level=9，副歌段动态）===\n");
    {
        music_t *m = make_music(1);
        for (int b = 0; b < MUSIC_MAX_BARS; b++)
            m->drum_level[b] = 9;
        size_t n = 0;
        int16_t *p = render_all(m, &n);
        free(m);
        if (p) {
            wstat_t s = wanalyze(p, n);
            wprint("level=9", &s, FS);
            free(p);
        }
    }

    /* ---------- 4. 削顶在时间上的分布：是不是集中在 hat 触发点 ---------- */
    printf("\n=== 4. 削顶的位置分布（每 125ms 一格，drum_step_ms=125）===\n");
    if (pcm_drum != NULL) {
        const size_t grid = (size_t)((double)FS * 0.125 + 0.5);
        static const uint8_t h[16] = {1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,1};
        static const uint8_t k[16] = {1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,0};
        static const uint8_t s[16] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,1};
        printf("  格   kick snare hat   削顶样点  格内peak\n");
        for (int g = 0; g < 16; g++) {
            size_t a = (size_t)g * grid;
            size_t b = a + grid;
            if (b > n_drum) b = n_drum;
            if (a >= b) break;
            size_t cl = 0; double pk = 0.0;
            for (size_t i = a; i < b; i++) {
                if (pcm_drum[i] >= 32767 || pcm_drum[i] <= -32768) cl++;
                double v = fabs((double)pcm_drum[i] / 32768.0);
                if (v > pk) pk = v;
            }
            printf("  %2d     %d     %d     %d     %8zu   %.4f %s\n",
                   g, k[g], s[g], h[g], cl, pk, cl ? "← 削顶" : "");
        }
    }

    free(pcm_nodrum);
    free(pcm_drum);

    /* ---------- 5. 大跳变的上下文（区分"噪声"与"硬切不连续"） ---------- */
    printf("\n=== 5. 大跳变的上下文（这是\"咔\"声的波形指纹）===\n");
    {
        struct { const char *nm; KS_DrumKind_t k; uint8_t lv; } cases[] = {
            { "snare level=3", KS_DRUM_SNARE, 3 },
            { "hat   level=3", KS_DRUM_HAT,   3 },
            { "kick  level=3", KS_DRUM_KICK,  3 },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            size_t n = 0;
            int16_t *p = render_one_drum(cases[c].k, cases[c].lv, &n);
            if (p == NULL) continue;
            dump_jumps(cases[c].nm, p, n, 0.30, 6);
            free(p);
        }
    }

    /* ---------- 6. 定位"不可能"的跳变（>1.0 全量程） ---------- */
    printf("\n=== 6. 超过 1.0 全量程的跳变（物理上不可能平滑，必是硬切）===\n");
    {
        struct { const char *nm; KS_DrumKind_t k; uint8_t lv; } cases[] = {
            { "kick  level=6", KS_DRUM_KICK,  6 },
            { "snare level=6", KS_DRUM_SNARE, 6 },
            { "snare level=9", KS_DRUM_SNARE, 9 },
            { "hat   level=9", KS_DRUM_HAT,   9 },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            size_t n = 0;
            int16_t *p = render_one_drum(cases[c].k, cases[c].lv, &n);
            if (p == NULL) continue;

            size_t worst_i = 0; double worst = 0.0;
            for (size_t i = 1; i < n; i++) {
                double d = fabs((double)p[i] - (double)p[i - 1]) / 32768.0;
                if (d > worst) { worst = d; worst_i = i; }
            }
            printf("  [%s] 最大跳变 %.4f @ i=%zu\n", cases[c].nm, worst, worst_i);

            /* 打印该位置前后的完整样点值（原始 int16，能直接看出是否被削平）*/
            if (worst > 0.9) {
                size_t a = (worst_i > 8) ? worst_i - 8 : 0;
                size_t b = worst_i + 8; if (b > n) b = n;
                printf("      前后原始样点：");
                for (size_t k = a; k < b; k++) {
                    printf(" %6d%s", p[k],
                           (p[k] >= 32767 || p[k] <= -32768) ? "*" : "");
                }
                printf("\n      （* 表示已到 int16 边界 = 被限幅削平）\n");
            }
            free(p);
        }
    }

    printf("\n============================================================\n");
    printf(" 诊断完成\n");
    printf("============================================================\n");
    return 0;
}
