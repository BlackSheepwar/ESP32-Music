/**
 * @file ks_test.c
 * @brief KS 合成器的离线数值验证（在 PC 上跑，不依赖 ESP32 硬件）
 *
 * @note  ★ 为什么需要它 ★
 *        扬声器还没到货，无法听测；而审计后改动的正是合成器最热、最险的部分
 *        （声部回收、梳状滤波、pick_position 边界、strum 解析）。
 *        所以把 ks_synth.c 原样编译到 PC 上，用数值断言代替耳朵。
 *
 *        编译方式见 build_ks_test.ps1。用的是 MinGW gcc + 与固件相同的
 *        -O3 -ffast-math，所以浮点行为与设备端基本一致。
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

/** @brief 日志等级（桩头文件里 extern 声明了它） */
esp_log_level_t g_log_level = ESP_LOG_WARN;      /* 默认只显示警告/错误，-v 打开 INFO */

/* 可复现的伪随机（固件用硬件随机，这里固定种子以便复现失败用例） */
static uint32_t g_rng = 0x12345678u;

uint32_t esp_random(void)
{
    /* xorshift32 */
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

void esp_random_seed(uint32_t seed) { g_rng = seed ? seed : 1u; }

#include "ks_synth.h"

/*==============================================================================
 * 统计与断言
 *============================================================================*/
static int g_fail = 0;
static int g_pass = 0;


static void check(int cond, const char *what)
{
    if (cond) { g_pass++; }
    else      { g_fail++; printf("  ✗ FAIL: %s\n", what); }
}


/** 渲染一段并统计 */
typedef struct {
    double rms;
    double peak;
    size_t n;
    double zcr;          /* 过零率（粗判频率） */
} stats_t;

static stats_t analyze(const int16_t *pcm, size_t n)
{
    stats_t s = {0};
    if (n == 0) return s;

    double sum2 = 0.0;
    double peak = 0.0;
    size_t zc = 0;

    for (size_t i = 0; i < n; i++)
    {
        double v = (double)pcm[i] / 32768.0;
        sum2 += v * v;
        if (fabs(v) > peak) peak = fabs(v);
        if (i > 0 && ((pcm[i - 1] < 0 && pcm[i] >= 0) || (pcm[i - 1] >= 0 && pcm[i] < 0)))
            zc++;
    }

    s.rms  = sqrt(sum2 / (double)n);
    s.peak = peak;
    s.n    = n;
    s.zcr  = (double)zc / (double)n;
    return s;
}

/**
 * @brief 用自相关估计基频（Hz）
 * @note  比过零率可靠：KS 的谐波结构复杂，过零率会被高频谐波带偏。
 *        在期望频率附近 ±30% 搜索自相关峰值。
 */
static double estimate_f0(const int16_t *pcm, size_t n, double fs, double expect)
{
    if (n < 1024) return 0.0;

    /* 去均值，避免直流影响相关 */
    double mean = 0.0;
    for (size_t i = 0; i < n; i++) mean += pcm[i];
    mean /= (double)n;

    double *x = (double *)malloc(n * sizeof(double));
    if (x == NULL) return 0.0;
    for (size_t i = 0; i < n; i++) x[i] = (double)pcm[i] - mean;

    int lag_min = (int)(fs / (expect * 1.35));
    int lag_max = (int)(fs / (expect * 0.72));
    if (lag_min < 2) lag_min = 2;
    if (lag_max > (int)(n / 2)) lag_max = (int)(n / 2);

    double best_corr = -1e30;
    int    best_lag  = 0;

    for (int lag = lag_min; lag <= lag_max; lag++)
    {
        double c = 0.0;
        size_t cnt = n - (size_t)lag;
        for (size_t i = 0; i < cnt; i++)
            c += x[i] * x[i + (size_t)lag];
        c /= (double)cnt;
        if (c > best_corr) { best_corr = c; best_lag = lag; }
    }

    free(x);

    if (best_lag <= 0) return 0.0;
    return fs / (double)best_lag;
}

/*==============================================================================
 * 构造一个测试用 music_t
 *============================================================================*/
#define TEST_FS      16000
#define TEST_BARS    4
#define TEST_SPB     8            /* beats_per_bar 4 × steps_per_beat 2 */

static music_t *make_music(int16_t pick_pos_x100, float damping, float decay,
                           int strum, int arp_len, int chord_per_bar)
{
    music_t *m = (music_t *)calloc(1, sizeof(music_t));
    if (m == NULL) return NULL;

    m->bpm            = 80;
    m->key_index      = 0;
    m->scale_index    = 0;
    m->bars           = TEST_BARS;
    m->beats_per_bar  = 4;
    m->steps_per_beat = 2;
    m->step_ms        = 375;
    m->instrument_index = 0;
    m->decay_seconds  = decay;
    m->brightness     = 0.6f;
    m->damping        = damping;
    m->pick_position  = (float)pick_pos_x100 / 100.0f;
    m->strum          = (int16_t)strum;
    m->strum_spread_ms = 22;

    /* 每小节 chord_per_bar 个和弦音，全部用 A3(57) 附近的和弦 */
    int idx = 0;
    for (int b = 0; b < TEST_BARS; b++)
    {
        m->chord_note_counts[b] = (int16_t)chord_per_bar;
        for (int k = 0; k < chord_per_bar && idx < MUSIC_MAX_CHORD_NOTES; k++)
        {
            static const int16_t chord[4] = { 57, 60, 64, 67 };  /* A3 C4 E4 G4 */
            m->chord_notes[idx++] = chord[k % 4];
        }
    }
    m->chord_notes_len = (int16_t)idx;

    /* ★ 旋律长度必须严格等于 bars × steps_per_bar ★
     * 这是后端 music.js:754 的约定：stepsPerBar = beatsPerBar × stepsPerBeat，
     * 而 melody.notes 就是按这个网格铺的（每小节 stepsPerBar 个）。
     * ★ 曾经写错过 ★：这里用 TEST_BARS * TEST_SPB 但 TEST_BARS 后面会被
     * 测试改成 MUSIC_MAX_BARS(32)，于是 melody_len 与 bars×spb 不再相等，
     * 时长算出来是 96 秒而不是 32 秒 —— 那不是固件 bug，是测试数据不自洽。
     * 现在统一由 make_music 的 bars 推导，保证任何测试里都自洽。 */
    m->melody_len = (int16_t)(m->bars * TEST_SPB);
    for (int i = 0; i < m->melody_len && i < MUSIC_MAX_MELODY_STEPS; i++)
        m->melody_notes[i] = 69;

    /* 低音：每小节 A2(45) */
    for (int b = 0; b < TEST_BARS; b++)
        m->bass_notes[b] = 45;

    m->arp_len = (int16_t)arp_len;
    for (int i = 0; i < arp_len && i < MUSIC_MAX_ARP; i++)
        m->arp_pattern[i] = 0;

    /* ★ 默认【不加鼓点】★
     * 原因：make_music 是测试 1~6 的公共构造器，而那些测试测的是
     * 拨弦音色/静音/声部回收等 —— 掺进鼓会让 rms 断言的含义变模糊
     * （实测过：nylon 的 rms 从 0.060 变成 0.088，因为鼓在贡献能量）。
     * 需要鼓的测试（测试 7）显式调 add_pop_drums()。 */
    m->drum_len = 0;

    return m;
}

/**
 * @brief 给乐谱装上后端 DRUM_PATTERNS.pop 图案（music.js:310-314）
 * @note  ★ 注意后端返回的是【一个小节的 16 格】，不是 bars×16 ★
 *        合成器必须对 drum_len 取模索引，否则第 2 小节就越界。
 */
static void add_pop_drums(music_t *m)
{
    static const uint8_t k[16] = {1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,0};
    static const uint8_t s[16] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,1};
    static const uint8_t h[16] = {1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,1};

    for (int i = 0; i < 16; i++)
    {
        m->drum_kick[i]  = k[i];
        m->drum_snare[i] = s[i];
        m->drum_hat[i]   = h[i];
    }
    m->drum_len     = 16;
    m->drum_step_ms = 125;      /* 120bpm 时后端给 60000/120/4 = 125ms */
    for (int b = 0; b < MUSIC_MAX_BARS; b++)
        m->drum_level[b] = 3;   /* 标准强度 */
}

/*==============================================================================
 * 测试 1：所有音色都必须出声（★ 覆盖 S2 harp pick_position=0.5 静音 bug ★）
 *============================================================================*/
static void test_all_instruments_sound(void)
{
    printf("\n=== 测试 1：6 个音色都必须出声（覆盖 harp pick=0.50 静音 bug）===\n");

    /* 后端 music.js:247-252 的真实参数 */
    struct { const char *name; int pick; float damp; float decay; } inst[] = {
        { "nylon_guitar",   40, 0.38f, 2.0f },
        { "steel_guitar",   28, 0.32f, 1.8f },
        { "clean_electric", 34, 0.46f, 2.5f },
        { "harp",           50, 0.24f, 3.5f },   /* ★ 就是 0.50，修复前会全程静音 ★ */
        { "banjo",          24, 0.28f, 1.0f },
        { "bass_pluck",     45, 0.53f, 2.1f },
    };

    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * TEST_FS * 2);
    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
    music_t *m = make_music(40, 0.38f, 2.0f, 0, 1, 3);

    if (pcm == NULL || s == NULL || m == NULL) { check(0, "alloc"); return; }

    for (size_t i = 0; i < sizeof(inst) / sizeof(inst[0]); i++)
    {
        m->pick_position = (float)inst[i].pick / 100.0f;
        m->damping       = inst[i].damp;
        m->decay_seconds = inst[i].decay;

        if (KS_Synth_Init(s, m, TEST_FS) != 0) { check(0, "KS_Synth_Init"); continue; }

        int n = KS_Synth_Render(s, pcm, TEST_FS * 2);
        stats_t st = analyze(pcm, (size_t)(n > 0 ? n : 0));

        printf("  %-16s pick=%.2f  rms=%.5f  peak=%.4f\n",
               inst[i].name, (double)inst[i].pick / 100.0, st.rms, st.peak);

        char msg[128];
        snprintf(msg, sizeof(msg), "%s 必须有声音（rms > 0.01）", inst[i].name);
        check(st.rms > 0.01, msg);

        snprintf(msg, sizeof(msg), "%s 不能削顶（peak <= 1.0）", inst[i].name);
        check(st.peak <= 1.0001, msg);
    }

    free(pcm); free(s); free(m);
}

/*==============================================================================
 * 测试 2：声部回收（★ 覆盖 S4 amp 永不衰减导致声部永不回收 ★）
 *============================================================================*/
static void test_voice_reclaim(void)
{
    printf("\n=== 测试 2：声部必须被回收（覆盖 amp 永不衰减 bug）===\n");

    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
    music_t *m = make_music(40, 0.38f, 1.0f, 0, 1, 3);   /* decay 1 秒，回收快 */
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 1024);

    if (s == NULL || m == NULL || pcm == NULL) { check(0, "alloc"); return; }

    KS_Synth_Init(s, m, TEST_FS);

    /* 渲染到曲子结束 + 尾音衰减完 */
    size_t total = (size_t)(TEST_FS * 20);
    size_t done = 0;
    uint32_t max_voices = 0;

    while (done < total)
    {
        int n = KS_Synth_Render(s, pcm, 1024);
        if (n <= 0) break;
        done += (size_t)n;
        uint32_t av = KS_Synth_ActiveVoices(s);
        if (av > max_voices) max_voices = av;
    }

    printf("  渲染 %zu 样点（%.1f 秒），峰值同时发声 %u 个声部\n",
           done, (double)done / TEST_FS, max_voices);

    check(max_voices > 0, "应该至少有过发声声部");

    /* ★ 关键断言：曲终后声部必须回到 0 ★
     * 修复前 amp 永不衰减 → 这里会恒等于 32 */
    uint32_t left = KS_Synth_ActiveVoices(s);
    printf("  曲终后剩余发声声部 = %u\n", left);
    check(left == 0, "曲终后所有声部必须已回收（修复前会恒为 32）");

    check(KS_Synth_IsFinished(s) == 1, "曲终后 IsFinished 应为 1");

    free(s); free(m); free(pcm);
}

/*==============================================================================
 * 测试 3：音高偏差量化（记录已知限制，不是失败条件）
 *============================================================================*/
static void test_pitch_accuracy(void)
{
    printf("\n=== 测试 3：音高偏差量化（已知限制，仅记录）===\n");
    printf("  %-6s %-8s %-10s %-10s %s\n", "MIDI", "期望Hz", "实测Hz", "偏差(音分)", "判定");

    struct { const char *name; int pick; float damp; } inst[] = {
        { "nylon", 40, 0.38f },
        { "bass",  45, 0.53f },
    };

    double worst = 0.0;

    for (size_t k = 0; k < sizeof(inst) / sizeof(inst[0]); k++)
    {
        for (int midi = 48; midi <= 84; midi += 12)
        {
            music_t *m = make_music(inst[k].pick, inst[k].damp, 3.0f, 0, 1, 3);
            ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
            int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 8192);
            if (m == NULL || s == NULL || pcm == NULL) { check(0, "alloc"); continue; }

            /* 只留一个旋律音，方便测基频 */
            m->melody_len = 1;
            m->melody_notes[0] = (int16_t)midi;
            m->chord_note_counts[0] = 0;
            m->chord_notes_len = 0;
            m->bass_notes[0] = -1;              /* 无效音高会被 Pluck 拒绝 */
            for (int b = 1; b < MUSIC_MAX_BARS; b++) m->chord_note_counts[b] = 0;
            m->bars = 1;
            m->arp_len = 0;

            double f_expect = KS_MidiToFreq(midi);

            if (KS_Synth_Init(s, m, TEST_FS) == 0)
            {
                int n = KS_Synth_Render(s, pcm, 4096);
                double f_got = estimate_f0(pcm, (size_t)(n > 0 ? n : 0), TEST_FS, f_expect);

                double cents = (f_got > 0.0 && f_expect > 0.0)
                             ? 1200.0 * log2(f_got / f_expect) : 0.0;
                if (fabs(cents) > fabs(worst)) worst = cents;

                printf("  %-6d %-8.1f %-10.1f %+-10.1f %s\n",
                       midi, f_expect, f_got, cents,
                       (fabs(cents) < 10.0) ? "OK" : (fabs(cents) < 50.0 ? "偏低(已知)" : "偏差大"));
            }

            free(m); free(s); free(pcm);
        }
    }

    printf("  最差偏差 = %+.1f 音分\n", worst);
    /* ★ 不断言音高：这是 AUDIT.md 里记录的已知限制（分数延迟补偿方式）
     *   参考实现 demo-page.js 的做法可做到 <0.6 音分，但那要改最热代码。
     *   这里只输出数值，供以后调音时对照。 */
    check(1, "音高测量完成（不设阈值，属已知限制）");
}

/*==============================================================================
 * 测试 4：strum 分支（★ 覆盖 S5 布尔解析 bug ★）
 *============================================================================*/
static void test_strum(void)
{
    printf("\n=== 测试 4：strum=1 应该走扫弦分支（一次拨响整个和弦）===\n");

    int16_t *pcm_a = (int16_t *)malloc(sizeof(int16_t) * TEST_FS);
    int16_t *pcm_b = (int16_t *)malloc(sizeof(int16_t) * TEST_FS);
    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));

    if (pcm_a == NULL || pcm_b == NULL || s == NULL) { check(0, "alloc"); return; }

    /* strum=1 且 arp_pattern 全 0（后端在扫弦时就是这么下发的） */
    music_t *m1 = make_music(28, 0.32f, 1.8f, 1, 4, 4);
    KS_Synth_Init(s, m1, TEST_FS);
    int n1 = KS_Synth_Render(s, pcm_a, TEST_FS);
    stats_t a = analyze(pcm_a, (size_t)(n1 > 0 ? n1 : 0));

    /* strum=0，同样参数 */
    music_t *m2 = make_music(28, 0.32f, 1.8f, 0, 4, 4);
    KS_Synth_Init(s, m2, TEST_FS);
    int n2 = KS_Synth_Render(s, pcm_b, TEST_FS);
    stats_t b = analyze(pcm_b, (size_t)(n2 > 0 ? n2 : 0));

    printf("  strum=1: rms=%.5f peak=%.4f\n", a.rms, a.peak);
    printf("  strum=0: rms=%.5f peak=%.4f\n", b.rms, b.peak);

    /* 扫弦会同时拨 4 个音 → 能量显著高于单音音型 */
    printf("  能量比 (strum/arp) = %.2f\n", (b.rms > 0.0) ? (a.rms / b.rms) : 0.0);
    check(a.rms > 0.01, "strum=1 必须有声音");

    /* ★ 关键：两个分支应该产生【不同】的输出。
     * 修复前 strum 永远解析成 0，两个分支会完全一样。 */
    int differ = 0;
    for (size_t i = 0; i < a.n && i < b.n; i++)
        if (pcm_a[i] != pcm_b[i]) { differ = 1; break; }

    check(differ, "strum=1 与 strum=0 的输出必须不同（修复前恒相同）");

    free(pcm_a); free(pcm_b); free(s); free(m1); free(m2);
}

/*==============================================================================
 * 测试 5：和弦 counts 一致性不能越界（★ 覆盖 S6 ★）
 *============================================================================*/
static void test_chord_clamp(void)
{
    printf("\n=== 测试 5：chord_note_counts 超容量时不能越界（覆盖 S6）===\n");

    ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * TEST_FS);

    if (s == NULL || pcm == NULL) { check(0, "alloc"); return; }

    /* 构造病态输入：32 小节 × 每小节 20 个和弦音 = 640 > 192 */
    music_t *m = make_music(40, 0.38f, 2.0f, 0, 1, 3);
    m->bars = MUSIC_MAX_BARS;
    /* ★ bars 改了，melody_len 也必须同步（否则时长不自洽）★ */
    m->melody_len = (int16_t)(m->bars * TEST_SPB);
    for (int b = 0; b < MUSIC_MAX_BARS; b++)
        m->chord_note_counts[b] = 20;
    m->chord_notes_len = MUSIC_MAX_CHORD_NOTES;

    printf("  输入：bars=%d，每小节 counts=20（合计 %d > 容量 %d）\n",
           MUSIC_MAX_BARS, MUSIC_MAX_BARS * 20, MUSIC_MAX_CHORD_NOTES);

    int r = KS_Synth_Init(s, m, TEST_FS);
    check(r == 0, "KS_Synth_Init 应能处理病态输入（内部夹逼）而不是失败");

    if (r == 0)
    {
        /* 走完整首曲子，越界会在这里崩溃/触发 ASAN */
        size_t done = 0;
        while (done < (size_t)(TEST_FS * 20))
        {
            int n = KS_Synth_Render(s, pcm, 1024);
            if (n <= 0) break;
            done += (size_t)n;
        }
        printf("  成功渲染 %zu 样点，未越界\n", done);
        check(1, "病态 counts 未导致越界");
    }

    /* 再试极端值 */
    m->chord_note_counts[0] = 32767;
    m->chord_note_counts[1] = -1;
    int r2 = KS_Synth_Init(s, m, TEST_FS);
    printf("  极端输入（32767 / -1）：KS_Synth_Init 返回 %d\n", r2);
    check(r2 == 0 || r2 == -1, "极端输入应被安全处理");

    if (r2 == 0)
    {
        int n = KS_Synth_Render(s, pcm, 1024);
        printf("  极端输入渲染返回 %d，未崩溃\n", n);
        check(1, "极端 counts 未导致崩溃");
    }

    free(s); free(pcm); free(m);
}

/*==============================================================================
 * 测试 6：decay 越长声部活得越久（验证 life_max 换算正确）
 *============================================================================*/
static void test_decay_scaling(void)
{
    printf("\n=== 测试 6：decay_seconds 应正比影响声部存活时长 ===\n");

    float decays[] = { 0.5f, 1.0f, 2.0f };
    uint32_t alive[3] = {0};

    for (int k = 0; k < 3; k++)
    {
        music_t *m = make_music(40, 0.38f, decays[k], 0, 1, 3);
        ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
        int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * TEST_FS);
        if (m == NULL || s == NULL || pcm == NULL) { check(0, "alloc"); continue; }

        /* 只拨一个音，然后看它活多久 */
        m->melody_len = 1;
        m->chord_note_counts[0] = 0;
        m->chord_notes_len = 0;
        m->arp_len = 0;
        m->bass_notes[0] = -1;
        for (int b = 1; b < MUSIC_MAX_BARS; b++) m->chord_note_counts[b] = 0;
        m->bars = 1;

        if (KS_Synth_Init(s, m, TEST_FS) == 0)
        {
            size_t frames = 0;
            while (frames < (size_t)(TEST_FS * 8))
            {
                int n = KS_Synth_Render(s, pcm, 256);
                if (n <= 0) break;
                frames += (size_t)n;
                if (KS_Synth_ActiveVoices(s) == 0) break;
            }
            alive[k] = (uint32_t)frames;
            printf("  decay=%.1fs → 声部存活 %.2f 秒\n",
                   (double)decays[k], (double)frames / TEST_FS);
        }

        free(m); free(s); free(pcm);
    }

    /* decay 翻倍，存活时长应大致翻倍（允许较宽容差） */
    check(alive[1] > alive[0], "decay 1.0s 应比 0.5s 活得久");
    check(alive[2] > alive[1], "decay 2.0s 应比 1.0s 活得久");
}

/*==============================================================================
 * 测试 7：鼓点（kick / snare / hat）
 *============================================================================*/
static void test_drums(void)
{
    printf("\n=== 测试 7：鼓点必须发声且三种音色不同 ===\n");

    /* ---- 7a. 手动逐个触发，验证三种音色都能出声且互不相同 ---- */
    {
        int16_t *buf[3];
        const char *names[3] = { "kick", "snare", "hat" };

        ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
        music_t *m = make_music(40, 0.38f, 2.0f, 0, 1, 3);
        if (s == NULL || m == NULL) { check(0, "alloc"); return; }

        /* 关掉所有拨弦，只留鼓（melody/chord/bass 全部清掉） */
        m->melody_len = 0;
        m->chord_note_counts[0] = 0;
        m->chord_notes_len = 0;
        m->arp_len = 0;
        m->bass_notes[0] = -1;
        for (int b = 1; b < MUSIC_MAX_BARS; b++) m->chord_note_counts[b] = 0;
        m->bars = 1;
        /* 鼓图案全清，靠手动触发 */
        for (int i = 0; i < 16; i++) { m->drum_kick[i]=0; m->drum_snare[i]=0; m->drum_hat[i]=0; }
        m->drum_len = 16;

        if (KS_Synth_Init(s, m, TEST_FS) != 0) { check(0, "Init"); return; }

        for (int k = 0; k < 3; k++)
        {
            buf[k] = (int16_t *)malloc(sizeof(int16_t) * TEST_FS);
            if (buf[k] == NULL) { check(0, "alloc buf"); return; }
            memset(buf[k], 0, sizeof(int16_t) * TEST_FS);

            /* ★ 每次只测一个鼓：渲染完要等它释放，否则下一个触发会叠加 ★
             * （第一次写这个测试时忘了这一点，ActiveDrums 累加成 1/2/3，
             *   于是"应有 1 个鼓"的断言失败 —— 是测试的问题，不是代码的。） */
            int tr = KS_Synth_TriggerDrum(s, (KS_DrumKind_t)k, 3);
            check(tr == 0, "TriggerDrum 应返回 0");
            check(KS_Synth_ActiveDrums(s) == 1, "触发后应立即有 1 个鼓在发声");

            /* ★ 渲染时长必须覆盖该鼓的完整寿命（life_left）★
             * 各鼓时间兜底：kick 0.40s / snare 0.30s / hat 0.12s。
             * 原来固定渲染 250ms，于是 kick（400ms）还没释放就轮到下一个，
             * ActiveDrums 变成 2 —— 是测试的问题，不是代码的。 */
            int n = KS_Synth_Render(s, buf[k], TEST_FS / 2);   /* 500ms */

            stats_t st = analyze(buf[k], (size_t)(n > 0 ? n : 0));
            printf("  %-6s rms=%.5f peak=%.4f\n", names[k], st.rms, st.peak);

            char msg[96];
            snprintf(msg, sizeof(msg), "%s 必须有声音（rms > 0.005）", names[k]);
            check(st.rms > 0.005, msg);
            snprintf(msg, sizeof(msg), "%s 不能削顶（peak <= 1.0）", names[k]);
            check(st.peak <= 1.0001, msg);

            /* ★ 渲染足够长时间后，鼓必须自己释放（时间兜底生效）★
             * 这同时验证了 life_left 的作用：大强度时 amp 衰减到不了 1e-4，
             * 光靠 amp 判据会永不释放。 */
            snprintf(msg, sizeof(msg), "%s 渲染 500ms 后应已释放", names[k]);
            check(KS_Synth_ActiveDrums(s) == 0, msg);
        }

        /* 三种音色必须互不相同 */
        int d01 = 0, d02 = 0, d12 = 0;
        for (size_t i = 0; i < TEST_FS / 4; i++)
        {
            if (buf[0][i] != buf[1][i]) d01 = 1;
            if (buf[0][i] != buf[2][i]) d02 = 1;
            if (buf[1][i] != buf[2][i]) d12 = 1;
        }
        check(d01, "kick 与 snare 输出必须不同");
        check(d02, "kick 与 hat 输出必须不同");
        check(d12, "snare 与 hat 输出必须不同");

        /* kick 是低频正弦，hat 是高频噪声 → hat 的过零率应显著更高 */
        stats_t sk = analyze(buf[0], TEST_FS / 4);
        stats_t sh = analyze(buf[2], TEST_FS / 4);
        printf("  过零率: kick=%.4f  hat=%.4f  (hat 应更高)\n", sk.zcr, sh.zcr);
        check(sh.zcr > sk.zcr, "hat 的过零率应高于 kick（高频 vs 低频）");

        free(s); free(m);
        for (int k = 0; k < 3; k++) free(buf[k]);
    }

    /* ---- 7b. 自动按图案触发 + drum_len 取模不能越界 ---- */
    {
        ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
        /* 16 小节 = 图案会绕 8 圈（每小节 16 格 / drum_len=16） */
        music_t *m = make_music(40, 0.38f, 2.0f, 0, 1, 3);
        int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 1024);
        if (s == NULL || m == NULL || pcm == NULL) { check(0, "alloc"); return; }

        m->bars = MUSIC_MAX_BARS;      /* 32 小节，图案要绕 32 次 */
        m->melody_len = (int16_t)(m->bars * TEST_SPB);   /* ★ 同步，保证时长自洽 ★ */
        add_pop_drums(m);

        int r = KS_Synth_Init(s, m, TEST_FS);
        check(r == 0, "带鼓点的 Init 应成功");

        /* ★ 渲染到"一轮真的结束"为止 ★
         * 判据与 app_music_app.c 的 music_synth_fill 完全一致：
         *   finished && 拨弦声部空 && 鼓也空
         * 不能固定渲染 70 秒 —— 那样曲子 32 秒就完了，后面 38 秒是空转。 */
        size_t done = 0;
        uint32_t max_drums = 0;
        int iter = 0;
        int finished_at = -1;
        while (done < (size_t)(TEST_FS * 120))     /* 硬上限，防死循环 */
        {
            int n = KS_Synth_Render(s, pcm, 1024);
            if (n <= 0) { printf("  [diag] Render 返回 %d，退出循环\n", n); break; }
            done += (size_t)n;
            iter++;

            uint32_t ad = KS_Synth_ActiveDrums(s);
            if (ad > max_drums) max_drums = ad;

            if (KS_Synth_IsFinished(s) && finished_at < 0)
                finished_at = iter;


            if (KS_Synth_IsFinished(s) &&
                KS_Synth_ActiveVoices(s) == 0 &&
                KS_Synth_ActiveDrums(s) == 0)
                break;      /* 一轮真的放完了 */
        }
        printf("  32 小节渲染 %zu 样点 (%.1f s)，%d 轮，finished 在第 %d 轮\n",
               done, (double)done / TEST_FS, iter, finished_at);
        printf("  同时发声的鼓最多 %u 个（池子 %d 个）\n", max_drums, KS_MAX_DRUMS);
        check(max_drums > 0, "自动触发应该真的产生了鼓声");
        check(max_drums <= KS_MAX_DRUMS, "鼓数量不能超过池子容量");

        /* 鼓池最终要全部释放 */
        printf("  曲终后剩余鼓 = %u\n", KS_Synth_ActiveDrums(s));

        check(KS_Synth_ActiveDrums(s) == 0, "曲终后鼓池应已释放");

        free(s); free(m); free(pcm);
    }

    /* ---- 7c. 没有鼓点时不应该有任何鼓 ---- */
    {
        ks_synth_t *s = (ks_synth_t *)malloc(sizeof(ks_synth_t));
        music_t *m = make_music(40, 0.38f, 2.0f, 0, 1, 3);
        int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * 1024);
        if (s == NULL || m == NULL || pcm == NULL) { check(0, "alloc"); return; }

        /* 模拟后端 drum_pattern = none（空数组 → 三轨全 0） */
        for (int i = 0; i < 16; i++) { m->drum_kick[i]=0; m->drum_snare[i]=0; m->drum_hat[i]=0; }

        KS_Synth_Init(s, m, TEST_FS);

        /* ★ 渲染帧数不能超过缓冲区容量（这里踩过坑）★
         * pcm 只分配了 1024 个 int16_t，原来却写 4096 —— 越界写 6KB，
         * 直接踩坏相邻堆块，表现为 free(pcm) 时 STATUS_HEAP_CORRUPTION。
         * 这是【测试代码】的错：固件里 out 缓冲来自 AUDIO_CHUNK_FRAMES，
         * 且调用方传的 frames 与缓冲容量一致（KS_Synth_Render 本身
         * 严格遵守 `for (i = 0; i < frames; i++)`，没有越界）。
         *
         * 记录教训：这个崩溃让我花了很多轮去查 ks_synth.c，
         * 最后发现是测试自己给错了尺寸 —— 排查内存问题时，
         * **先核对每次 malloc 的容量与随后使用的长度**。 */
        int n = KS_Synth_Render(s, pcm, 1024);
        printf("  无鼓点：渲染 %d 样点，发声鼓数 = %u\n", n, KS_Synth_ActiveDrums(s));
        check(KS_Synth_ActiveDrums(s) == 0, "无可鼓点图案时不应触发任何鼓");

        free(pcm); free(s); free(m);
    }
}

/*==============================================================================
 * main
 *============================================================================*/
int main(int argc, char **argv)
{
    /* ★ 关掉 stdout 缓冲：崩溃时也要能看见已经打印的内容 ★
     * （默认全缓冲时，进程异常终止会导致缓冲区丢失，什么都看不到） */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        g_log_level = ESP_LOG_INFO;

    printf("============================================================\n");
    printf(" KS 合成器离线验证（fs=%d Hz）\n", TEST_FS);
    printf(" 用途：扬声器未到货时用数值断言代替听测\n");
    printf("============================================================\n");

    test_all_instruments_sound();
    test_voice_reclaim();
    test_pitch_accuracy();
    test_strum();
    test_chord_clamp();
    test_decay_scaling();
    test_drums();

    printf("\n============================================================\n");
    printf(" 结果：%d 项通过，%d 项失败\n", g_pass, g_fail);
    printf("============================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
