/**
 * @file app_music.c
 * @brief 音乐应用层实现（cJSON 解析后端响应）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 解析策略：只认 music 字段，其余全部跳过。
 *       ★ 后端 API.md §3.1 的约定：「设备只读 music，人只读 labels」
 *         所以本文件里看不到任何 labels / intent / intent_raw 的处理 —— 这是有意的。
 *
 *       ★ 所有数组都按「先读长度、再逐个填、超容量就截断并报告」的方式处理，
 *         不做任何假设长度的 memcpy。因为后端曲长由环境变量 TOTAL_BARS 控制
 *         （4~32），是不可编译期确定的，硬编码长度迟早会踩越界。
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
#include "app_music.h"
#include "app_music_cfg.h"
// 功能包含
#include "cJSON.h"
#include "esp_log.h"

/*==============================================================================
 * 宏定义与常量
 *============================================================================*/
static const char *TAG = "APP_MUSIC";

/*==============================================================================
 * 内部函数：安全取值
 *============================================================================*/
/**
 * @brief 取整数字段（字段缺失时保留默认值）
 */
static int music_get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item))
        return item->valueint;
    return fallback;
}

/**
 * @brief 取浮点字段
 */
static float music_get_float(const cJSON *obj, const char *key, float fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item))
        return (float)item->valuedouble;
    return fallback;
}

/**
 * @brief 取布尔字段（★ 审计修复：strum 是 JSON 布尔，不是数字 ★）
 * @param obj      JSON 对象
 * @param key      键名
 * @param fallback 字段缺失时的默认值
 * @retval 0 或 1
 *
 * @note  ★ 为什么不能用 music_get_int ★
 *        music_get_int 靠 `cJSON_IsNumber` 判断，而 ESP-IDF 绑定的 cJSON 里
 *        布尔解析后的 type 是 `cJSON_True`（1<<1），与 `cJSON_Number`（1<<3）
 *        是【不同的位】→ IsNumber 对布尔返回 false → 永远取到 fallback。
 *        后端 music.js:792 返回的正是 `strum`（布尔：
 *        `arpKey === 'strum' || intent.strum === true`）。
 *        后果很严重：后端给"扫弦"时会同时给 `arp_pattern = [0,0,0,0]`
 *        （music.js:284），固件走普通音型分支 → `tone = 0 % count = 0`
 *        → 【每一格都拨同一个根音】，正是 music.js:726-728 注释里说
 *        "听着很怪"、后端已经修掉的那个行为。
 *
 *        这里同时兼容 1/0 的数字写法（有些版本的后端可能给数字）。
 */
static int music_get_bool_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsBool(item))
        return cJSON_IsTrue(item) ? 1 : 0;

    if (cJSON_IsNumber(item))
        return (item->valueint != 0) ? 1 : 0;

    return fallback;
}

/**
 * @brief 把整数夹到 [lo, hi]，非法值（NaN 经 int 转换后不可判）用 fallback
 * @note  ★ 为什么必须有它 ★
 *        cJSON 对超出 int 范围的数字会饱和成 INT_MAX / INT_MIN，
 *        直接赋值给 int16_t 字段会截断成完全无关的值。
 *        而后端字段会进入时间轴乘法、数组下标、滤波系数，
 *        任何一个不对都可能造成未定义行为或越界。详见 App_Music_Parse 里的说明。
 */
static int music_clamp_int(int v, int lo, int hi, int fallback)
{
    if (v < lo || v > hi)
        return fallback;
    return v;
}

/**
 * @brief 把浮点夹到 [lo, hi]，NaN 用 fallback
 */
static float music_clamp_float(float v, float lo, float hi, float fallback)
{
    /* NaN 的特征：自己不等于自己。必须先挡掉，否则下面的比较全是 false，
     * 会把 NaN 原样放行，然后进入 powf / 滤波器就发散。 */
    if (v != v)
        return fallback;
    if (v < lo || v > hi)
        return fallback;
    return v;
}

/**
 * @brief 取整型数组到 int16 缓冲
 * @param obj   JSON 对象
 * @param key   字段名
 * @param out   输出缓冲
 * @param cap   容量
 * @retval 实际写入的元素个数（数组缺失时为 0）
 * @note  ★ 超容量时只填前 cap 个并打警告，不越界 ★
 */
static int music_get_int_array(const cJSON *obj, const char *key,
                               int16_t *out, int cap)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr))
        return 0;

    int n = cJSON_GetArraySize(arr);
    int written = 0;

    for (int i = 0; i < n && written < cap; i++)
    {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(e))
            continue;
        out[written++] = (int16_t)e->valueint;
    }

    if (n > cap)
    {
        ESP_LOGW(TAG, "field '%s' has %d items but capacity is %d, truncated", key, n, cap);
    }

    return written;
}

/**
 * @brief 取 0/1 小数组到 uint8 缓冲（鼓点用）
 */
static int music_get_u8_array(const cJSON *obj, const char *key,
                             uint8_t *out, int cap)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(arr))
        return 0;

    int n = cJSON_GetArraySize(arr);
    int written = 0;

    for (int i = 0; i < n && written < cap; i++)
    {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(e))
            continue;
        out[written++] = (uint8_t)e->valueint;
    }

    return written;
}

/*==============================================================================
 * 解析
 *============================================================================*/
int App_Music_Parse(const char *json, music_t *out)
{
    if (json == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));

    /* ---------- 解析 JSON ---------- */
    /* ★ cJSON_Parse 会自己申请内存放整棵语法树，解析完必须 Delete。
     *   响应约 3~10KB，语法树内存约 3~5 倍，内部 RAM 完全够（约 30~50KB）。★ */
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse failed near: %.60s", (err != NULL) ? err : "(unknown)");
        return -2;
    }

    int ret = 0;

    do
    {
        /* ---------- 顶层 ok ---------- */
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        if (!cJSON_IsBool(ok) || !cJSON_IsTrue(ok))
        {
            /* ★ 失败时后端也会带完整诊断，把 error 和 hint 打出来 ★ */
            const cJSON *err_item  = cJSON_GetObjectItemCaseSensitive(root, "error");
            const cJSON *hint_item = cJSON_GetObjectItemCaseSensitive(root, "hint");

            ESP_LOGE(TAG, "backend returned ok=false: %s",
                     (cJSON_IsString(err_item) && err_item->valuestring) ? err_item->valuestring : "(no error field)");
            if (cJSON_IsString(hint_item) && hint_item->valuestring)
                ESP_LOGE(TAG, "  hint: %s", hint_item->valuestring);

            ret = -3;
            break;
        }

        /* ---------- music 字段 ---------- */
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "music");
        if (!cJSON_IsObject(m))
        {
            ESP_LOGE(TAG, "no 'music' object in response");
            ret = -4;
            break;
        }

        /* ---- 时间基准 ----
         * ★ 每个字段都必须 clamp（审计发现的缺陷）★
         *   cJSON 对超范围数字会饱和成 INT_MAX/INT_MIN，直接 (int16_t) 转换会
         *   截断成任意值（例如 1e30 → INT_MAX → (int16_t)-1）。
         *   而 step_ms / beats_per_bar / steps_per_beat 都会进入时间轴乘法，
         *   负值或极大值会造成未定义行为（负 double 转 uint32_t）或溢出。
         *   所以这里统一走 clamp_int()。 */
        out->bpm            = (int16_t)music_clamp_int(music_get_int(m, "bpm", 90),             20, 300, 90);
        out->key_index      = (int16_t)music_clamp_int(music_get_int(m, "key_index", 0),         0, 11,  0);
        out->scale_index    = (int16_t)music_clamp_int(music_get_int(m, "scale_index", 0),       0, 7,   0);
        out->bars           = (int16_t)music_clamp_int(music_get_int(m, "bars", 4),              1, MUSIC_MAX_BARS, 4);
        out->beats_per_bar  = (int16_t)music_clamp_int(music_get_int(m, "beats_per_bar", 4),     1, 16,  4);
        out->steps_per_beat = (int16_t)music_clamp_int(music_get_int(m, "steps_per_beat", 2),    1, 8,   2);
        out->step_ms        = (int16_t)music_clamp_int(music_get_int(m, "step_ms", 333),         1, 4000, 333);

        /* ---- 和声 ---- */
        out->chord_notes_len = (int16_t)music_get_int_array(m, "chord_notes",
                                                            out->chord_notes, MUSIC_MAX_CHORD_NOTES);
        music_get_int_array(m, "chord_note_counts", out->chord_note_counts, MUSIC_MAX_BARS);
        music_get_int_array(m, "bass_notes",        out->bass_notes,        MUSIC_MAX_BARS);

        /* ★★ 关键校验：把 chord_note_counts 夹进 chord_notes 的实际长度之内 ★★
         *
         * 这是审计发现的【唯一严重缺陷】，原因是这个组合决定了 KS 侧的读取偏移：
         *     base  = 前面所有小节 count 之和        （ks_synth.c 的前缀和）
         *     index = base + tone                    （tone < 本小节 count）
         * chord_notes 只有 MUSIC_MAX_CHORD_NOTES(192) 个槽，而 counts 是后端下发的
         * 任意整数。music_get_int_array() 只保证**写入端**不越界（written < cap），
         * **完全不校验元素值**。所以：
         *   · 合法输入也能触发：32 小节 × 每小节 7 个音 = 224 > 192 → 越界读
         *   · 畸形响应更糟：counts 给 32767，或 cJSON 把 1e30 饱和成 INT_MAX
         *     再截成 -1 → 前缀和累加到 ±100 万 → 地址偏移 ±2MB → LoadProhibited 崩溃
         *
         * 修法：逐小节夹到"剩余可用和弦音数"里，同时把负值归零。
         * 这里修好之后，KS 侧还会再做一次防御（见 ks_synth.c），双保险。 */
        {
            int remain = (int)out->chord_notes_len;

            for (int b = 0; b < MUSIC_MAX_BARS; b++)
            {
                int c = (int)out->chord_note_counts[b];

                if (c < 0)
                    c = 0;                  /* 负数当 0（cJSON 饱和可能产生 -1） */
                if (c > remain)
                    c = remain;             /* 不许超过剩余可用槽位 */
                if (c > MUSIC_MAX_CHORD_NOTES)
                    c = MUSIC_MAX_CHORD_NOTES;

                out->chord_note_counts[b] = (int16_t)c;
                remain -= c;
                if (remain <= 0)
                {
                    /* 后面的小节一律给 0，防止前缀和继续增长 */
                    for (int k = b + 1; k < MUSIC_MAX_BARS; k++)
                        out->chord_note_counts[k] = 0;
                    break;
                }
            }

            if (remain < 0)
                remain = 0;

            ESP_LOGD(TAG, "chord counts clamped, %d note slots left unused", remain);
        }

        /* ---- 伴奏音型 ---- */
        out->arp_len = (int16_t)music_get_int_array(m, "arp_pattern",
                                                    out->arp_pattern, MUSIC_MAX_ARP);
        /* ★ strum 是布尔，必须用 music_get_bool_int（审计修复 S5）★ */
        out->strum           = (int16_t)music_get_bool_int(m, "strum", 0);
        out->strum_spread_ms = (int16_t)music_get_int(m, "strum_spread_ms", 0);

        /* ---- 旋律 ---- */
        out->melody_len = (int16_t)music_get_int_array(m, "melody_notes",
                                                       out->melody_notes, MUSIC_MAX_MELODY_STEPS);
        music_get_int_array(m, "melody_hold", out->melody_hold, MUSIC_MAX_MELODY_STEPS);

        /* ---- 音色（KS 参数）---- */
        out->instrument_index = (int16_t)music_clamp_int(music_get_int(m, "instrument_index", 0), 0, 31, 0);
        /* ★ 浮点参数必须 clamp ★
         *   decay_seconds 会进 powf(0.001, 1/(fs*gain_decay))：
         *   若为 0 或负数 → 除零 / 非法指数；极大值 → 反馈系数退化成 1（自激）。
         *   damping / pick_position 已进 KS 的滤波系数，超范围会让滤波器发散。 */
        out->decay_seconds    = music_clamp_float(music_get_float(m, "decay_seconds", 3.0f),  0.1f, 30.0f, 3.0f);
        out->brightness       = music_clamp_float(music_get_float(m, "brightness", 0.6f),     0.0f, 1.0f,  0.6f);
        out->damping          = music_clamp_float(music_get_float(m, "damping", 0.3f),        0.0f, 0.95f, 0.3f);
        out->pick_position    = music_clamp_float(music_get_float(m, "pick_position", 0.28f), 0.0f, 0.5f,  0.28f);

        /* ---- 鼓点（可选）---- */
        out->drum_step_ms = (int16_t)music_get_int(m, "drum_step_ms", 0);
        int dk = music_get_u8_array(m, "drum_kick",  out->drum_kick,  MUSIC_MAX_DRUM_STEPS);
        int ds = music_get_u8_array(m, "drum_snare", out->drum_snare, MUSIC_MAX_DRUM_STEPS);
        int dh = music_get_u8_array(m, "drum_hat",   out->drum_hat,   MUSIC_MAX_DRUM_STEPS);
        music_get_u8_array(m, "drum_level", out->drum_level, MUSIC_MAX_BARS);
        out->drum_len = (int16_t)((dk > ds) ? ((dk > dh) ? dk : dh) : ((ds > dh) ? ds : dh));

        /* ---------- 一致性检查 ---------- */
        if (out->bars > MUSIC_MAX_BARS)
        {
            ESP_LOGW(TAG, "bars=%d exceeds capacity %d, truncated", out->bars, MUSIC_MAX_BARS);
            out->bars = MUSIC_MAX_BARS;
            ret = -5;
        }
        if (out->arp_len == 0)
        {
            /* 没有音型就没法做伴奏，兜一个根音 */
            out->arp_pattern[0] = 0;
            out->arp_len = 1;
            ESP_LOGW(TAG, "arp_pattern missing, fallback to root-only");
        }
        if (out->chord_notes_len == 0)
        {
            ESP_LOGE(TAG, "chord_notes is empty, nothing to synthesize");
            ret = -4;
            break;
        }

        ESP_LOGI(TAG, "parsed: %d bars, %d bpm, step=%d ms, chords=%d, melody=%d, arp=%d, strum=%d",
                 out->bars, out->bpm, out->step_ms,
                 out->chord_notes_len, out->melody_len, out->arp_len, out->strum);

    } while (0);

    cJSON_Delete(root);
    return ret;
}

/*==============================================================================
 * 摘要打印
 *============================================================================*/
void App_Music_Dump(const music_t *m)
{
    if (m == NULL)
        return;

    ESP_LOGI(TAG, "==================== music ====================");
    ESP_LOGI(TAG, "  bpm=%d  key=%d  scale=%d  bars=%d  %d/%d per bar",
             m->bpm, m->key_index, m->scale_index, m->bars,
             m->beats_per_bar, m->steps_per_beat);
    ESP_LOGI(TAG, "  step_ms=%d  ->  duration=%u ms (%.1f s)",
             m->step_ms, (unsigned)Music_DurationMs(m),
             (double)Music_DurationMs(m) / 1000.0);
    ESP_LOGI(TAG, "  chords: %d notes in %d bars, first bar has %d",
             m->chord_notes_len, m->bars,
             (m->bars > 0) ? m->chord_note_counts[0] : 0);
    ESP_LOGI(TAG, "  bass:   [%d %d %d %d ...]",
             m->bass_notes[0],
             (m->bars > 1) ? m->bass_notes[1] : 0,
             (m->bars > 2) ? m->bass_notes[2] : 0,
             (m->bars > 3) ? m->bass_notes[3] : 0);
    ESP_LOGI(TAG, "  arp:    len=%d [%d %d %d %d ...]  strum=%d spread=%dms",
             m->arp_len,
             m->arp_pattern[0],
             (m->arp_len > 1) ? m->arp_pattern[1] : 0,
             (m->arp_len > 2) ? m->arp_pattern[2] : 0,
             (m->arp_len > 3) ? m->arp_pattern[3] : 0,
             m->strum, m->strum_spread_ms);
    ESP_LOGI(TAG, "  melody: len=%d  first 8:", m->melody_len);
    for (int i = 0; i < 8 && i < m->melody_len; i++)
        ESP_LOGI(TAG, "    [%d] note=%d hold=%d", i, m->melody_notes[i], m->melody_hold[i]);
    ESP_LOGI(TAG, "  instrument=%d  decay=%.2fs  brightness=%.2f  damping=%.2f  pick=%.2f",
             m->instrument_index, (double)m->decay_seconds,
             (double)m->brightness, (double)m->damping, (double)m->pick_position);
    ESP_LOGI(TAG, "  drums: step=%dms len=%d level[0]=%d",
             m->drum_step_ms, m->drum_len,
             (m->bars > 0) ? m->drum_level[0] : 0);
    ESP_LOGI(TAG, "===============================================");
}
