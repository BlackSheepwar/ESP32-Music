# ESP32 智能音乐生成系统

海外客户硬件原型 / 小批量定制项目的实操题实现。

```
麦克风语音  ->  ESP32  --HTTPS-->  Cloudflare Worker  --Workers AI-->  ASR + 意图解析
                     <--JSON----                     <--乐理展开--
                                                                          |
ESP32  Karplus-Strong 合成  <---  BPM / 调式 / 和弦 / 节奏 / 旋律  --------+
```

**本仓库当前进度：云端已完成并测试通过（151 项断言）。固件端待硬件到货。**

---

## 目录结构

```
esp32-music/
├── README.md                    本文件
├── docs/
│   └── API.md                   ★ 给固件端的 API 契约（含 C 代码示例）
└── worker/                      Cloudflare Worker（云端）
    ├── package.json
    ├── wrangler.toml            配置：STAGE / DEVICE_TOKEN / AI 绑定
    ├── src/
    │   ├── index.js             主逻辑：路由 + WAV 诊断 + 四阶段
    │   ├── music.js             确定性乐理展开（和弦符号 -> MIDI 数组）
    │   └── demo-page.js         浏览器录音测试台（内联，随 Worker 部署）
    └── test/
        └── local.mjs            离线测试，不需要 wrangler / 账号 / 联网
```

---

## 快速开始

```bash
cd worker
npm install
npm test          # 离线跑，151 项断言，不需要账号
```

`npm test` 全通过说明代码逻辑没问题，可以放心部署。

---

## 部署

```bash
cd worker

# 1. 登录 Cloudflare（会打开浏览器做 OAuth 授权）
npx wrangler login

# 2. 先本地预览
npx wrangler dev
#   -> 打开 http://localhost:8787 就是录音测试台
#   注意：本地 dev 时 AI 绑定会走云端真实模型，需要已登录

# 3. 部署
npx wrangler deploy
#   -> 输出 https://music-intent-api.<你的子域>.workers.dev
```

部署后**第一件事**：把返回的地址填进 `docs/API.md` 和固件里，然后打开这个地址——
你会看到录音测试台。

### 免费额度

| 项目 | 免费额度 | 你的实际用量 |
|---|---|---|
| Workers 请求 | 10 万/天 | 开发期每天几百次 |
| **CPU 时间** | **10 ms/请求** | 见下方「设计约束」 |
| 请求体 | 100 MB | 160 KB / 次 |
| Workers AI | 10,000 Neurons/天 | Whisper ≈ 47/音频分钟，LLM ≈ 12/次 |

按 5 秒一段算，Whisper 的额度够 **每天约 2500 次**，开发演示完全用不完。

---

## 四个阶段（用 `wrangler.toml` 里的 `STAGE` 切换）

这是本项目最重要的调试设计。**每次只让一个变量变化。**

| STAGE | 用途 | 调不调 AI |
|---|---|---|
| `mock` | **硬件没到货时开发 ESP32 端**。返回一份固定的结构化音乐数据 | ✗ |
| `echo` | **麦克风调试**。回显 WAV 头 + RMS + 诊断建议 | ✗ |
| `asr` | 只跑语音识别，验证 Whisper 效果 | 仅 Whisper |
| `full` | 最终形态：识别 + 意图解析 + 乐理展开 | Whisper + LLM |

改完 `STAGE` 后 `npx wrangler deploy` 十秒生效。

### `echo` 阶段为什么重要

它把 **「麦克风硬件问题」** 和 **「云端问题」** 变成了可区分的事件。
后端会直接返回这样的诊断：

```json
{
  "audio": {
    "rms": 0.0001,
    "hints": [
      "RMS 极低（几乎静音）：优先怀疑 INMP441 的 L/R 脚悬空或接错、
       I2S DIN 引脚接错、或 slot_mask 没有设成 LEFT"
    ]
  }
}
```

没有这个，你会花一整天猜到底哪一层出了问题。

---

## 调试

```bash
npx wrangler tail --format pretty     # 实时看云端日志
```

配合 `request_id`（响应里的 8 位 ID）可以在日志里精确找到某一次请求。

---

## 两个关键设计决策

### 1. 乐理放在云端，不放设备端

LLM 只输出**符号层**（`"key": "G"`, `"chords": ["Gmaj7","Em7","Am7","D7"]`），
Worker 里的 `music.js` 用**确定性代码**展开成 MIDI 音高数组。

好处：
- LLM 在数值音乐理论上经常出错，符号层出错一眼能看出来
- 展开是纯函数，同样输入永远同样输出，**可以写单元测试**（已覆盖）
- **ESP32 端不需要任何乐理代码** —— 拿到 `chord_notes: [55,59,62,66,...]` 直接合成
- 改乐理逻辑只需 `wrangler deploy`，不用重新烧固件

### 2. 设备只读 `music`，人只读 `labels`

响应里这两块是分开的。固件**完全不用解析 `labels`**，省掉一堆字符串处理。

### 3. Karplus-Strong 的衰减参数做了重新参数化

后端下发的是 **`decay_seconds`（衰减到 −60dB 的秒数）**，不是反馈系数。

原因：如果直接下发每周期反馈系数，延迟线长度 `N = 采样率/频率` 会让**低音衰减比高音快**，
和真实弦乐器正好相反。换成衰减秒数后，每样点系数 `d = 0.001^(1/(采样率 × decay_seconds))`
**与频率无关**，所有音高在同样时间里衰减。

推导写在 `src/music.js` 的音色表上方，C 代码实现在 `docs/API.md` §5。

---

## 设计约束：10ms CPU 上限

Cloudflare Workers 免费版**每请求只有 10ms CPU 时间**。

但官方明确：**等待网络 I/O 不计入 CPU 时间**。所以：

- ✅ 调 Whisper 等 3 秒 —— 是 I/O 等待，安全
- ✅ 调 LLM 等 2 秒 —— 安全
- ❌ **在 Worker 里做音频解码 / 重采样 / FFT / DSP —— 必死**

所以架构上：**ESP32 直接输出后端能吃的格式（16kHz 单声道 16bit WAV），
Worker 只做转发和编排。**

当前实现里唯一有 CPU 成本的是 base64 编码（160 KB ≈ 2–5 ms），已经用分块方式实现。
如果后续加功能后超限，两个选择：缩短录音时长，或升 Workers Paid（$5/月，上限提到 30 秒）。

---

## 测试覆盖

`npm test` 的 151 项断言覆盖：

- **和弦解析**：14 种符号写法、转位、别名、延伸音降级、非法输入拒绝
- **乐理展开**：音域边界、数组长度、确定性可复现、小调调内和弦正确性、越界钳制
- **WAV 解析**：合法/畸形/空/超短/非 PCM 格式、RMS 与峰值数值正确性
- **音频诊断**：静音检测、采样率异常提示
- **四阶段路由**：每个阶段调用哪些 AI、mock 阶段零 AI 调用
- **base64 编解码**：字节级无损验证
- **容错**：LLM 输出无法解析时兜底、AI 抛异常时返回 502、非法 WAV 不浪费 AI 调用
- **鉴权与限额**：token 校验、音频时长上限
- **HTTP 路由**：health / 测试页 / 404 / 405 / CORS 预检
- **测试页内联脚本语法**：把 `<script>` 内容交给 JS 引擎解析，能发现模板字符串转义错误

> 测试已经抓到并修复了 3 个真实缺陷：`parseChord('garbage')` 被静默当成 G 大三和弦、
> `Cmaj9` 丢失七度音、**小调兜底走向里 `Cmin7` 不是调内和弦**（A 小调的 III 级应是 Cmaj7）。

---

## 下一步

1. **现在就能做**：`wrangler login` + `deploy`，用浏览器测试台录一段话，
   验证「说一句话 -> 拿到结构化音乐 -> 试听」这条链
2. **硬件到货后**：按 `docs/API.md` 实现固件端
   - I2S 录音（注意 32bit 槽 + `I2S_STD_SLOT_LEFT`）
   - WAV 封装
   - `esp_http_client` POST（**记得 `timeout_ms = 30000`**）
   - Karplus-Strong 合成（C 代码在 `docs/API.md` §5）
3. **调优**：用测试台试听，把 `decay_seconds` / `brightness` 调到好听，再搬到 C 代码
