# API 契约

> 这是**固件端要照着实现的唯一权威文档**。改后端前先改这里，两边保持一致。

---

## 1. 基本信息

| 项 | 值 |
|---|---|
| 方法 | `POST` |
| 路径 | `/api/v1/intent` |
| 协议 | **HTTPS**（ESP-IDF 用 `esp_crt_bundle_attach`，不需要手动塞证书） |
| 请求 Content-Type | `audio/wav` |
| 请求体 | 原始 WAV 文件字节（不是 base64，不是 multipart） |
| 认证 | `Authorization: Bearer <DEVICE_TOKEN>`（后端未配置 token 时可省略） |
| 响应 | JSON，UTF-8 |

**响应体积**（2026-10 实测，与本文档早期版本的数字不同，已更正）：

| 场景 | 体积 |
|---|---|
| `TOTAL_BARS=16`（默认）+ 美化输出 | **约 6.2 KB** |
| `TOTAL_BARS=16` + `?pretty=0` 紧凑输出 | **约 2.6 KB** |
| `TOTAL_BARS=32` + 紧凑输出 | **约 10 KB** |

→ **固件端接收缓冲建议 16 KB**（早期写的「≥4 KB」是按当时更短的曲子估的，会截断）。
→ 固件端请在 URL 上带 **`?pretty=0`**，体积减半、TLS 时间减半。

完整样例见 [`sample-response.json`](./sample-response.json) —— 它由 `worker/test/sample.mjs`
从真实代码跑出来生成，不是手写的，所以字段名和结构一定和实际返回一致。

> ⚠️ **本文档曾落后于代码**，已按 `src/index.js` / `src/music.js` 修正三处：
> 响应体积（本节）、`music` 的字段列表（见 §3.3）、曲长上限（`bars` 实际是 4~32 而不是 1~8）。
> 另补充两个文档原先没写的接口：`GET /api/v1/health`（探活，见 §3.5）与 `?pretty=0`。

---

## 2. 请求：音频格式

固件端必须输出**标准 WAV**：

| 参数 | 值 | 说明 |
|---|---|---|
| 容器 | RIFF/WAVE | 44 字节标准头 |
| 编码 | PCM，`audioFormat = 1` | 不接受浮点或压缩格式 |
| 采样率 | **16000 Hz** | Whisper 原生采样率，省一次重采样 |
| 声道 | **1（单声道）** | |
| 位深 | **16 bit** | 小端有符号 |
| 时长 | 建议 2–8 秒 | 上限由后端 `MAX_AUDIO_SECONDS` 控制，默认 15 秒 |

5 秒音频 = 44 + 160000 = **160044 字节**。

### INMP441 的转换

INMP441 输出 24bit 数据、左对齐放在 32bit 槽里。I2S 按 `I2S_DATA_BIT_WIDTH_32BIT` 读取后：

```c
int32_t raw = rx_buf[i];
int16_t sample = (int16_t)(raw >> 16);      // 取高 16 位
// 如果实测 RMS 偏低（< 0.01），改用 raw >> 14 获得 4 倍数字增益
```

I2S 配置必须是：

```c
.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
// 然后显式指定左声道 —— 默认是 I2S_STD_SLOT_BOTH，会读到一半空数据
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // 对应 INMP441 的 L/R 脚接地
```

---

## 3. 响应

### 3.1 顶层字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | bool | 成功为 `true` |
| `stage` | string | 后端当前阶段：`mock` / `echo` / `asr` / `full` |
| `request_id` | string | 8 位随机 ID，排查问题时用它去 `wrangler tail` 里搜 |
| `text` | string | 语音识别文本（`mock` 阶段是占位文字） |
| `audio` | object | 音频诊断信息（见 3.2） |
| `music` | object | **设备要读的数字层**（见 3.3） |
| `labels` | object | 人类可读的标签层（见 3.4），**设备可以完全忽略** |
| `intent` | object | LLM 的原始输出，仅 `full` 阶段有，调试用 |
| `timing` | object | 各阶段耗时（毫秒） |

> **设计约定：设备只读 `music`，人只读 `labels`。**
> 固件里不要解析 `labels`，能省掉一堆字符串处理。

### 3.2 `audio`（诊断用，可选读）

| 字段 | 说明 |
|---|---|
| `bytes` | 收到的字节数 |
| `ok` | WAV 是否合法 |
| `reason` | 不合法时的原因 |
| `sampleRate` / `channels` / `bitsPerSample` | 解析出的格式 |
| `durationSec` | 时长 |
| `rms` | 均方根音量，**调增益就看这个** |
| `peak` | 峰值 |
| `dc` | 直流偏置 |
| `hints` | **字符串数组，直接告诉你下一步改什么** |

`hints` 是最有用的字段。典型输出：

```
"RMS 极低（几乎静音）：优先怀疑 INMP441 的 L/R 脚悬空或接错、I2S DIN 引脚接错、
 或 slot_mask 没有设成 LEFT（默认 BOTH 会读到一半空数据）"
```

### 3.3 `music`（设备必读）

> ★ 本表已按 `src/music.js` 的实际返回更正（早期版本漏了 `bass_notes`、
> `strum`、`strum_spread_ms`、`damping`、`pick_position` 和全部鼓点字段）★

| 字段 | 类型 | 说明 |
|---|---|---|
| `bpm` | int | 速度，40–220 |
| `key_index` | int | 调式主音，0=C, 1=C#, …, 11=B |
| `scale_index` | int | 音阶，见 §4.2 |
| `bars` | int | 小节数，**4–32**（由环境变量 `TOTAL_BARS` 控制，默认 16） |
| `beats_per_bar` | int | 每小节拍数，默认 4 |
| `steps_per_beat` | int | 每拍几格（2 = 八分音符），默认 2 |
| `step_ms` | int | **一格的毫秒数，已预计算**。`step_ms = 60000 / bpm / steps_per_beat` |
| `chord_notes` | int[] | 所有小节的和弦音高（MIDI），**扁平拼接** |
| `chord_note_counts` | int[] | 每小节各有多少个和弦音 |
| `bass_notes` | int[] | **每小节一个低音（MIDI，已低两个八度）** —— 加上它音乐才「有底」 |
| `arp_pattern` | int[] | 伴奏音型，元素是「和弦音的下标」，循环使用 |
| `strum` | bool | **1 = 扫弦**：每格一次性拨响该小节所有和弦音 |
| `strum_spread_ms` | int | 扫弦时各弦错开的毫秒数（后端给 22） |
| `melody_notes` | int[] | 主旋律 MIDI 音高，**`-1` 表示休止** |
| `melody_hold` | int[] | 每个音保持多少格 |
| `instrument_index` | int | 音色，见 §4.3 |
| `decay_seconds` | float | 衰减到 −60dB 的秒数，见 §5 |
| `brightness` | float | 激励亮度 0..1，见 §5 |
| `damping` | float | **扩展 KS：环路阻尼**（高频消失速度，独立于总延音），见 §5.4 |
| `pick_position` | float | **扩展 KS：拨弦位置 0..0.5**，见 §5.4 |
| `drum_pattern` | string | 鼓型标签（`none` / `pop` / …），仅给人看 |
| `drum_step_ms` | int | 鼓点每格的毫秒数（每小节固定 16 个十六分音符，与 `steps_per_beat` 无关） |
| `drum_kick` / `drum_snare` / `drum_hat` | int[] | 每格 0/1 |
| `drum_level` | int[] | 每小节的鼓点强度 0~9（段落动态） |
| `sections` | object[] | 曲式结构 `{name, bars}`，设备端不需要，给前端画结构用 |

> ★ `melody_notes` 的长度 = `bars × beats_per_bar × steps_per_beat`，
> 在 `TOTAL_BARS=32` 时可达 256 个元素。固件端数组容量要按 32 小节留。

### 3.4 `labels`（人读，设备忽略）

`key`（如 `"G"`）、`scale`（如 `"major"`）、`chords`（如 `["Gmaj7","Em7","Am7","D7"]`）、`instrument`、`style`。

---

## 4. 查表（固件端必须镜像这些表）

> ⚠️ **这些表的已有顺序不可变更，只能往后追加。** 后端也会遵守这条约定，
> 否则老固件会解析出错误的和弦。

### 4.1 和弦类型 `CHORD_TYPES`

`chord_types` 的含义体现在 `chord_notes` 里（后端已经把和弦展开成 MIDI 音高了），
所以固件**通常不需要这张表**。列在这里是为了完整性。

| index | key | 音程（半音） |
|---|---|---|
| 0 | maj | 0, 4, 7 |
| 1 | min | 0, 3, 7 |
| 2 | maj7 | 0, 4, 7, 11 |
| 3 | min7 | 0, 3, 7, 10 |
| 4 | dom7 | 0, 4, 7, 10 |
| 5 | dim | 0, 3, 6 |
| 6 | aug | 0, 4, 8 |
| 7 | sus4 | 0, 5, 7 |
| 8 | sus2 | 0, 2, 7 |
| 9 | six | 0, 4, 7, 9 |
| 10 | min6 | 0, 3, 7, 9 |
| 11 | m7b5 | 0, 3, 6, 10 |
| 12 | add9 | 0, 4, 7, 14 |

### 4.2 音阶 `SCALES`

| index | key | 音级（半音） |
|---|---|---|
| 0 | major | 0, 2, 4, 5, 7, 9, 11 |
| 1 | minor | 0, 2, 3, 5, 7, 8, 10 |
| 2 | harmonic_minor | 0, 2, 3, 5, 7, 8, 11 |
| 3 | dorian | 0, 2, 3, 5, 7, 9, 10 |
| 4 | mixolydian | 0, 2, 4, 5, 7, 9, 10 |
| 5 | pentatonic_major | 0, 2, 4, 7, 9 |
| 6 | pentatonic_minor | 0, 3, 5, 7, 10 |
| 7 | blues | 0, 3, 5, 6, 7, 10 |

> 主旋律已由后端生成，固件一般也不需要音阶表。

### 4.3 音色 `INSTRUMENTS`

| index | key | decay_seconds | brightness |
|---|---|---|---|
| 0 | nylon_guitar | 4.0 | 0.60 |
| 1 | steel_guitar | 3.0 | 0.78 |
| 2 | clean_electric | 5.0 | 0.45 |
| 3 | harp | 8.0 | 0.35 |
| 4 | banjo | 1.2 | 0.85 |
| 5 | bass_pluck | 3.5 | 0.30 |

### 4.4 伴奏音型 `ARP_PATTERNS`

| key | 数组 |
|---|---|
| arp_up | 0,1,2,3 |
| arp_updown | 0,1,2,3,2,1 |
| arp_wave | 0,1,2,3,2,1,2,3 |
| arp_16th | 0,1,2,3 ×4 |
| fingerpick | 0,2,1,3,0,2,1,3 |
| travis | 0,2,1,3,0,3,1,2 |
| strum | 0,0,0,0 |
| bass_only | 0 |

---

## 5. Karplus–Strong：怎么用返回的参数

### 5.1 反馈系数（关键，别写错）

后端下发的是 **`decay_seconds`（衰减到 −60dB 的秒数）**，不是反馈系数本身。

固件端换算：

```c
// 每样点反馈系数。注意它【与频率无关】——
// 所有音高都会在同样的墙钟时间里衰减到 -60dB。
// 推导：N = 采样率/频率（延迟线长度），P = 频率 × decay_seconds（周期数）
//       d = (0.001^(1/P))^(1/N) = 0.001^(1/(采样率 × decay_seconds))
float ks_decay_coeff(float sample_rate, float decay_seconds) {
    return powf(0.001f, 1.0f / (sample_rate * decay_seconds));
}
```

> **为什么不用「每周期衰减系数」当参数？**
> 因为那样低音会比高音死得更快（N 大 → 衰减快），和真实弦乐器相反。
> 换成衰减秒数后系数与频率无关，每个音色只需算一次。

### 5.2 合成循环

```c
#define KS_MAX_N 512              // 16kHz 下最低音 C3 的 N ≈ 122，512 有充足余量

typedef struct {
    float  buf[KS_MAX_N];
    int    n;                     // 延迟线长度
    int    idx;
    float  d;                     // 每样点反馈系数
    float  amp;                   // 包络（用于释放）
} ks_voice_t;

// MIDI -> 频率
static inline float midi_to_freq(float midi) {
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}

// 拨弦：初始化激励
void ks_pluck(ks_voice_t *v, float freq, float sample_rate,
              float brightness, float decay_seconds) {
    v->n = (int)(sample_rate / freq + 0.5f);
    if (v->n < 2) v->n = 2;
    if (v->n > KS_MAX_N) v->n = KS_MAX_N;
    v->idx = 0;
    v->amp = 1.0f;
    v->d = ks_decay_coeff(sample_rate, decay_seconds);

    // 激励：一阶低通白噪声。brightness 越大保留高频越多 -> 拨弦越"脆"
    float a = brightness;
    float prev = 0.0f;
    for (int i = 0; i < v->n; i++) {
        float white = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        prev = a * prev + (1.0f - a) * white;
        v->buf[i] = prev;
    }
}

// 每采样步进一次
static inline float ks_next(ks_voice_t *v) {
    float cur = v->buf[v->idx];
    int nxt = (v->idx + 1) % v->n;
    v->buf[v->idx] = v->d * 0.5f * (cur + v->buf[nxt]);
    v->idx = nxt;
    return cur;
}
```

### 5.3 内存

延迟线极短：16kHz 下最低音 C3（130.8Hz）只需要 N = 122 个 float。

**8 个声部 × 512 float × 4 字节 = 16 KB。** 多声部复音几乎不花代价，
所以「6 根弦 + 主旋律」完全可以同时发声。这部分甚至可以放在内部 RAM 里。

---

## 6. 播放时间轴

```
step_duration = music.step_ms / 1000.0   秒
第 i 格的发生时间 = 启动时刻 + i * step_duration
```

### 伴奏

```c
int steps_per_bar = beats_per_bar * steps_per_beat;
int total_steps   = MAX(melody_len, bars * steps_per_bar);

// 先算每小节和弦音在 chord_notes 里的起始下标
int offset[8];
int acc = 0;
for (int b = 0; b < bars; b++) { offset[b] = acc; acc += chord_note_counts[b]; }

for (int i = 0; i < total_steps; i++) {
    int bar     = i / steps_per_bar;
    int pat_idx = i % arp_len;
    int tone    = arp_pattern[pat_idx] % chord_note_counts[bar];
    int midi    = chord_notes[offset[bar] + tone];
    // 在第 i 格触发 midi
}
```

### 主旋律

```c
for (int i = 0; i < melody_len; i++) {
    if (melody_notes[i] < 0) continue;        // -1 = 休止，不发声
    // 在第 i 格触发 melody_notes[i]
}
```

---

## 7. 错误码

| HTTP | `error` | 含义与处理 |
|---|---|---|
| 400 | `empty request body` | 没发 body |
| 400 | `invalid wav: <原因>` | WAV 头不对。**先看 `audio.hints`** |
| 401 | `unauthorized` | token 不对 |
| 405 | `use POST` | 用了 GET |
| 413 | `audio too long: Xs > Ys` | 录音太长 |
| 502 | `asr failed: <原因>` | Whisper 失败，重试一次 |
| 200 | `ok: false` | body 里 `ok` 为 false，看 `error` 字段 |

**注意**：WAV 非法时后端会返回 400，但 body 里**仍然带完整的 `audio.hints`** —— 这是故意的，
让固件端在串口里直接打印出「该改哪里」。

---

## 8. 命令行测试

```bash
BASE=https://music-intent-api.<你的子域>.workers.dev

# 探活
curl $BASE/api/v1/health

# 用真实录音测试（把 test.wav 换成你的录音）
curl -X POST --data-binary @test.wav \
     -H "Content-Type: audio/wav" \
     $BASE/api/v1/intent

# 带 token
curl -X POST --data-binary @test.wav \
     -H "Content-Type: audio/wav" \
     -H "Authorization: Bearer <DEVICE_TOKEN>" \
     $BASE/api/v1/intent
```

---

## 9. 固件端实现要点（ESP-IDF）

### ⚠️ 必踩的坑 1：超时默认值太小

`esp_http_client` 默认 `timeout_ms = 5000`。而真实链路耗时：

```
上传 160KB       0.5–1.5s
Whisper 推理     1–3s
LLM 推理         1–3s
────────────────────────
合计             3–8s
```

**症状**：串口报 timeout，但 `wrangler tail` 显示 Worker 成功返回 200。
两边日志矛盾，极难排查。

→ **必须设 `timeout_ms = 30000`。**

```c
esp_http_client_config_t config = {
    .url = "https://music-intent-api.xxx.workers.dev/api/v1/intent",
    .method = HTTP_METHOD_POST,
    .timeout_ms = 30000,                        // ★ 不能省
    .crt_bundle_attach = esp_crt_bundle_attach, // ★ Cloudflare 证书在 bundle 里
};
```

### ⚠️ 必踩的坑 2：响应体大于接收缓冲

默认 RX buffer 只有 512 字节，而响应约 2 KB。

→ 用 event handler 累积 `HTTP_EVENT_ON_DATA`：

```c
static char resp[4096];
static int  resp_len;

static esp_err_t http_event(esp_http_client_event_t *e) {
    switch (e->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp_len + e->data_len < sizeof(resp) - 1) {
            memcpy(resp + resp_len, e->data, e->data_len);
            resp_len += e->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        resp[resp_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}
```

### 发送二进制 body

用 `esp_http_client_set_post_field`，**不要**用字符串拼接：

```c
esp_http_client_set_header(client, "Content-Type", "audio/wav");
esp_http_client_set_post_field(client, (const char *)wav_buf, wav_len);
esp_http_client_perform(client);
```

### 认证头

```c
esp_http_client_set_header(client, "Authorization", "Bearer " DEVICE_TOKEN);
```

> 注意：当前 `wrangler.toml` 里 `DEVICE_TOKEN = ""`，即**不校验**，
> 所以现在不发这个头也能用。以后开了鉴权再带上。

---

## 9.5 探活接口（文档原先漏了，固件端很有用）

```
GET /api/v1/health
```

响应：

```json
{
  "ok": true,
  "service": "music-intent-api",
  "stage": "full",
  "auth_required": false,
  "ai_binding": true,
  "asr_model": "@cf/openai/whisper-large-v3-turbo",
  "llm_model": "@cf/meta/llama-3.1-8b-instruct-fp8-fast",
  "ts": "2026-10-13T09:55:34.014Z"
}
```

**为什么固件端该用它**：想判断「后端到底通不通」时，不要发一个空 POST 去试 ——
那会走完整链路浪费 AI 额度。`/health` 不调 AI、不读 body、响应只有几百字节，
最适合做「是否连接后端」的指示灯数据源。
`auth_required` 字段还能告诉你现在到底要不要带 token。

### 其它可用端点（调试用，固件端不需要）

| 端点 | 用途 |
|---|---|
| `GET /` 或 `GET /demo` | 浏览器录音测试页（硬件没到货时做端到端验证） |
| `GET /api/v1/aitest` | 只调一次最便宜的 LLM，验证 AI 链路本身通不通 |
| `GET /api/v1/aitest?asr=1` | 用一段内置 1 秒 WAV 测 Whisper |
| `GET /api/v1/aitest?intent=<文本>` | 用真实系统提示词跑一次意图解析，原样吐出模型输出 |

---

## 10. 建议的 C 结构体

只保留设备真正需要的字段：

```c
#define MAX_BARS          8
#define MAX_CHORD_NOTES   48
#define MAX_MELODY_STEPS  128
#define MAX_ARP           16

typedef struct {
    int   bpm;
    int   key_index;
    int   scale_index;
    int   bars;
    int   beats_per_bar;
    int   steps_per_beat;
    int   step_ms;

    int   chord_notes[MAX_CHORD_NOTES];
    int   chord_notes_len;
    int   chord_note_counts[MAX_BARS];

    int   arp_pattern[MAX_ARP];
    int   arp_len;

    int   melody_notes[MAX_MELODY_STEPS];
    int   melody_hold[MAX_MELODY_STEPS];
    int   melody_len;

    int   instrument_index;
    float decay_seconds;
    float brightness;
} music_t;
```

用 `cJSON` 解析要点：

- `melody_notes` 里的 `-1` 是**休止**，不是错误值
- `chord_notes` 是扁平的，配合 `chord_note_counts` 切分各小节
- `labels` 整块跳过不解析
- 解析完记得 `cJSON_Delete(root)`

---

## 11. 前端到端时序预算

| 环节 | 耗时 |
|---|---|
| I2S 录音 5 秒 | 5.0 s |
| TLS 握手（首次） | 0.3–1.0 s |
| 上传 160 KB | 0.5–1.5 s |
| Whisper | 1–3 s |
| LLM | 1–3 s |
| 下载响应 2 KB | 0.05 s |
| **总计** | **约 8–14 秒** |

**给用户的心理预期**：说完话到开始出声大约 10 秒。
建议固件在等待期间点亮 LED 或播放一个「思考中」的音效，否则用户会以为设备死机了。
