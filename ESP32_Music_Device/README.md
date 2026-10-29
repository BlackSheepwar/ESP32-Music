# ESP32 音乐设备端（device side）

基于 **ESP32-S3-N16R8** + ESP-IDF 5.4.4 的智能音乐设备固件。
配合后端 `esp32-music`（Cloudflare Worker）工作，接口契约见
`../esp32-music/docs/API.md`。

**当前状态**：设备端功能已全部实现并可编译通过（1,027,968 字节，占 2MB 分区的 50%）。
**WiFi 已实测连通**，**后端接口已实测打通**（`GET /health` 与 `POST /intent` 均返回 200，上传耗时 2 秒、响应 2769 字节、返回 16 小节 / 48 秒完整曲子）。
麦克风与扬声器尚未到货，音频通路待实物验证。

---

## 1. 快速上手

```powershell
# 1) 改两个宏
#    User/APP/APP_WIFI/app_wifi_cfg.h    → WIFI_SSID / WIFI_PASSWORD
#    User/APP/APP_MUSIC/app_music_cfg.h  → BACKEND_BASE_URL

cd D:\a1\Project\Study\ESP32_Music_Device

# 2) 编译 + 烧录
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor

# 如果 idf.py 报 PermissionError: [WinError 5]，用工程自带的脚本代替：
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

**接线表、指示灯对照、后端地址怎么查、故障排查 → 见 [`HARDWARE.md`](./HARDWARE.md)**

---

## 2. 硬件

| 部件 | 型号 | 接口 | 状态 |
|---|---|---|---|
| 开发板 | ESP32-S3-N16R8（16MB flash + 8MB Octal PSRAM） | — | 已到位 |
| 麦克风 | INMP441 | I2S0（只收） | **待到货** |
| 扬声器 | MAX98357A | I2S1（只发） | **待到货** |
| 指示灯 | 普通 LED × 5（语义灯） | GPIO，高电平点亮 | 已到位 |
| 按键 | 轻触开关 × 4 | GPIO，低电平按下 | 已到位 |

---

## 3. 软件分层

上层只拿 **BSP 暴露的具名句柄 / APP 的逻辑编号**操作硬件，不传引脚号、不传端口号：

```c
App_LED_Set(APP_LED_MIC, &APP_LED_PAT_ON);      /* 不是 gpio_set_level(GPIO_NUM_6, 1) */
BSP_I2S_Read(g_i2s_mic, buf, n, 1000);          /* 不是 i2s_channel_read(chan0, ...) */
BSP_HTTP_PostWav(g_http_api, wav, len, ...);    /* 不是 esp_http_client_perform() */
```

引脚写死在 BSP 层的映射表里（`GPIO_HW_MAP` / `I2S_HW_MAP`），
句柄结构体对上层不透明，所以 BSP 头文件不依赖 ESP-IDF，换板子只改那张表。

```
main.c                    app_main：打印芯片信息 + 初始化 APP + 建任务 + 监控内存
 │
 ├─ User/TASK/            任务层：建 FreeRTOS 任务、周期调度、生产-消费
 │    TASK_AUDIO/         硬实时（prio 6, core1）唯一操作 I2S：录音采集 + 播放推进
 │    TASK_KEY/           扫描（10ms）+ 命令分发（阻塞等事件）→ 4 键业务
 │    TASK_WIFI/          50ms（core0）WiFi 状态机 + 后端探活 + 后端指示灯
 │    TASK_LED/           LED 服务（10ms）+ 生命灯（500ms 纯翻转）
 │
 ├─ User/APP/             应用层：状态机 + 业务判断
 │    APP_AUDIO/          录音缓冲 + 统一播放任务（可插拔音源）
 │    APP_LED/            非阻塞闪烁状态机（图案表）+ 指示灯配置
 │    APP_KEY/            消抖状态机 + 事件队列
 │    APP_WIFI/           重连退避策略 + WiFi 指示灯联动   ← WiFi 配置宏
 │    APP_MUSIC/          JSON 解析 + 业务流程编排 + 后端 URL 宏
 │    APP_LEDTEST/        引脚扫描自检（默认关闭，诊断用）
 │
 ├─ User/Middlewares/     纯算法中间件（只依赖 Common）
 │    KS/                 Karplus–Strong 拨弦合成器
 │
 ├─ User/BSP/             硬件抽象层：句柄式接口，引脚写死在映射表里
 │    BSP_GPIO/           5 语义灯 + 4 按键 + 2 预留
 │    BSP_I2S/            INMP441 采集 + MAX98357 播放 + WAV 封装
 │    BSP_WIFI/           esp_wifi STA 封装，状态回调抛给上层
 │    BSP_HTTP/           HTTPS 客户端（crt_bundle / 30s 超时 / 16KB 响应）
 │    BSP_SYS/            时间基准、芯片信息
 │
 └─ User/Common/          平台无关：共享数据类型 + 工具
      ds_common.h        错误码、ARRAY_SIZE、float_to_str、bit_onehot_index
      ds_music.h         music_t（★ APP 与 Middlewares 的共享类型，见下）
```

### ★ 为什么要有一层 `Common/ds_music.h`

`music_t` 被两个不同层的模块同时需要：KS 合成器要读它合成，
JSON 解析器要填它。如果定义在 APP 层，Middlewares 就得依赖 APP，
而 APP 又需要 Middlewares 的 KS —— **组件循环依赖，CMake 直接报错**。
把共享的**数据类型**下沉到 Common 是最干净的解法（Common 不依赖任何组件）。

### ★ 头文件命名约定：必须带 `ds_` 前缀

踩过一次：`common.h` 被 ESP-IDF 的 `mbedtls/library/common.h` **静默顶掉**，
报出「`ARRAY_SIZE` 隐式声明」这种完全误导人的错误（用 `gcc -H` 才看出来）。
同样地 `ERR_OK` / `ERR_TIMEOUT` 和 lwIP 的 `err.h` 撞名。
**所以工程自己的全局可见标识符一律加 `ds_` / `DS_` 前缀。**

### 任务优先级设计

| 任务 | 优先级 | 周期 | 核心 | 为什么 |
|---|---|---|---|---|
| `task_audio` | 6 | 事件驱动 | 1 | I2S DMA 只有约 90ms 缓冲，读晚了丢样点（爆音） |
| `task_key_cmd` | 5 | 事件驱动 | 1 | 按键响应要快；上传时阻塞 3~10 秒是有意的 |
| `task_key` | 5 | 10ms | 1 | 扫描周期必须稳定，否则长按判定不准 |
| `task_wifi` | 4 | 50ms | 0 | 与 WiFi 协议栈同核；探活阻塞不影响事件处理 |
| `task_led` | 1 | 10ms | 1 | 灯不紧急 |
| `task_heartbeat` | 1 | 500ms | 1 | 生命灯：只有延时+翻转，代码路径越短越可靠 |

---

## 4. 已实现功能

### 4.1 五个语义指示灯

| 灯 | 引脚 | 行为 |
|---|---|---|
| 生命灯 | GPIO4 | **500ms 方波**，独立任务翻转（证明系统还活着） |
| WiFi | GPIO5 | 灭 / 快闪(连接中) / **常亮(已连上)** / 慢闪(重连) / 双闪(长退避) |
| 录音 | GPIO6 | 亮=**KEY1** 正在录音（KEY3 回环录音不点灯） |
| 有音频 | GPIO7 | **灭=还没有可播放的**；**常亮=有**；载入新音频时闪三下 |
| 后端 | GPIO8 | 灭(WiFi没连) / **快闪(URL没就绪 —— 动作项提示)** / 常亮(可达) / 慢闪(不可达) |

> ★ **"有音频"灯的时序是明确的**（按用户要求）★
>
> ```
> 按 KEY1 录音 → 松手 → 灯【灭】→ 上传中（灯保持灭）→ 后端返回数据
>                                                      └→ 灯【常亮】+ 闪三下
> ```
>
> **灯不会在"录完音"就亮** —— 它只在**后端真的把音乐数据返回来、
> 合成器初始化成功**之后才亮。上传失败或被取消时灯保持灭，
> 如实反映"没有新的可播放音频"。
>
> 上传进度由**后端指示灯**和串口日志承担，不借用这个灯 ——
> 否则"灭"这个状态会变得含糊（究竟是没有音频，还是正在传？）。

### 4.2 四个按键

| 键 | 引脚 | 功能 |
|---|---|---|
| KEY1 | GPIO21 | **按住录音**（松手停止并上传后端 → 解析 → 准备合成）；录满 8 秒自动停。**按下即取消正在进行的上一次上传** |
| KEY2 | GPIO38 | **循环播放**合成音频 / 再按停止（下次从头开始） |
| KEY3 | GPIO39 | **回环测试**：**按住录音 / 松手停止**（不上传，只存本地）；**不操作 LED**，松手后串口打印 ms/peak/dc/clipped + 判读结论 |
| KEY4 | GPIO40 | **回环回放**：按一下从头播一遍（不循环）；播放中再按停止；停止后再按从头开始。**不操作 LED** |

> ★ **上传在独立任务里执行，不阻塞按键**（重要）★
>
> 上传最坏要 45 秒 × 5 次重试 ≈ 3.75 分钟。它由 `Task_Key_Upload` 执行，
> `Task_Key_Cmd` 只投一条信号就立刻返回 —— 否则按键事件队列（16 深）会溢出，
> 丢掉状态转换事件，表现为「**第一次按 KEY1 能录、第二次按没反应**」。
>
> ★ **再录一段会顶掉上一次上传**（按用户要求）★
> 上传中按 KEY1 录第二段，会**取消**第一次的上传，然后上传第二段。
>
> ⚠️ 取消是**协作式**的，不是立刻掐断连接 —— 实测
> `esp_http_client_write` 的一次调用内部没有回调点，正在传输的那一次无法中断。
>
> | | 生效时机 |
> |---|---|
> | ✅ 立刻 | 不再重试（省掉最多 4 次 × 45 秒 ≈ 3 分钟） |
> | ⏳ 需等待 | 当前这次传输最多再跑 45 秒才结束 |
>
> 详见 `bsp_http.h` 里 `BSP_HTTP_RequestCancel()` 的说明。

### 4.3 WiFi（STA 客户端模式）

- SSID/密码写在 `app_wifi_cfg.h` 宏里，固定连一个 AP
- 状态机：连接中 → 已连接 → 掉线重连（1/2/4/8/8s 退避）→ 长退避（5/10/20/30s，永不放弃）
- **不用 ESP-IDF 自带的自动重连**（与退避策略叠加会出现「疯狂重连」被路由器拉黑）
- 状态以**拿到 IP** 为准，不是以「关联成功」为准（这个坑很常见）
- 配置只存 RAM 不写 NVS；连接成功后每 10 秒巡检防事件漏报

### 4.4 音频驱动

- INMP441（I2S0）+ MAX98357（I2S1），两个独立主机口
  （不共用一个全双工口，因为 MAX98357 对 BCLK 占空比敏感）
- 固定 16000Hz / 单声道 / 16bit，直接对齐后端 Whisper 输入格式
- 已处理 INMP441 的两个坑：24bit 左对齐塞 32bit 槽、L/R 接地必须用 `SLOT_LEFT`
- 录音缓冲（8 秒 = 256KB）与回环缓冲（5 秒 = 160KB）放 **PSRAM**
- 扬声器上电预热 100ms 静音（MAX98357 要先收到时钟才使能输出）

### 4.5 统一播放任务 + 可插拔音源

三类要发声的东西（回环 PCM / KS 实时合成 / 未来 WAV）共用一个播放任务：

```c
typedef struct App_AudioSource {
    int  (*fill)(struct App_AudioSource *self, int16_t *out, size_t frames);
    void (*rewind)(struct App_AudioSource *self);
    void *user;
} App_AudioSource_t;
```

好处：
- 不会出现两个任务同时写 I2S（DMA 数据交错 → 声音全乱）
- 播放节流靠 `i2s_channel_write` 阻塞，不需要自己凑时序
- **KS 合成不需要预先渲染整首曲子**（42 秒 = 1.3MB），边播边算，PSRAM 占用为 0

### 4.6 Karplus–Strong 合成器
按 `API.md §5` 的公式实现，并**补上了文档没写的两个后端扩展参数**：

| 参数 | 来源 | 实现 |
|---|---|---|
| `decay_seconds` | 文档 §5.1 | `d = 0.001^(1/(fs×decay))`，**与频率无关**（文档的推导是对的） |
| `damping` | **文档没有，music.js 有** | 写入延迟线前的一阶低通（单位直流增益形式，不影响 decay 时间） |
| `pick_position` | **文档没有，music.js 有** | 激励的梳状滤波（拨弦位置决定谐波分布，吉他音色的关键） |
| `brightness` | 文档 §5.2 | 激励的一阶低通 |
| `strum` / `strum_spread_ms` | **文档没有，music.js 有** | 扫弦时同时拨响所有和弦音，按递减增益近似错开 |
| 分数延迟 | 实现细节 | 用写入低通的相位延迟补偿（`a = 1 - 1/(frac+1)`），把音高误差从 ~7 音分压下来 |

另外：32 复音（和弦 + 低音 + 旋律 + 余音）、激励必须去直流（否则延迟线里的直流会永远绕圈）、
停止走短释放包络而不是直接清零（否则有「咔」声）。

### 4.6.1 鼓点合成（kick / snare / hat）

**后端早就在下发鼓点数据**（`drum_kick` / `drum_snare` / `drum_hat` / `drum_step_ms` / `drum_level`），
之前固件只解析不使用。现在按后端给出的配方实现（`demo-page.js:555-621`）：

| 音色 | 配方 | 包络 |
|---|---|---|
| **kick** 底鼓 | 正弦波，音高在 60ms 内从 **120Hz 滑到 45Hz**（"砰"的关键） | 0.30s |
| **snare** 军鼓 | 高通噪声（1300Hz）+ **190Hz 三角波**（鼓皮，"没有它就只有沙没有啪"） | 0.19s / 0.13s |
| **hat** 踩镲 | 极短高通噪声（**4500Hz**，见下方说明） | 0.055s |

**采样率适配**（唯一偏离参考实现的地方）：

| 项 | 参考（48kHz） | 本工程（16kHz） |
|---|---|---|
| kick 滑音 120→45Hz | — | 不变（频率与采样率无关） |
| snare 高通 | 1300Hz | 不变（完全在带内） |
| hat 高通 | 7200Hz | ⚠️ **改成 4500Hz** |

> hat 必须下调：16kHz 的奈奎斯特只有 8000Hz，7200Hz 高通会把几乎所有能量滤掉。

**密度**：`drum_step_ms = 60000/bpm/4`（16 分音符），是 `step_ms` 的**一半** ——
所以鼓点比和弦密一倍（120bpm 时 125ms vs 250ms）。

**索引要点**：后端返回的是**一个小节的 16 格图案**（不是 `bars×16`），
所以必须 `idx % drum_len` 取模，否则第 2 小节就越界。

**验证**（离线测试，`tools/run_ks_test.ps1`）：

```
kick   rms=0.06627 peak=0.4289
snare  rms=0.03743 peak=0.4720
hat    rms=0.00827 peak=0.1527
过零率: kick=0.0065  hat=0.1560   ← hat 高 23 倍，符合"低频 vs 高频"预期
曲终后剩余鼓 = 0
```

过零率这一项证明三种音色是**真的合成了不同频段的声音**，而不是同一个噪声换了音量。

### 4.7 后端对接

| 项 | 实现 |
|---|---|
| 后端地址 | **`https://www.230508.xyz`**（✅ 自定义域名，**国内直连实测可用**，见 §4.8） |
| 上传 | `POST {BASE}/api/v1/intent?pretty=0`，`Content-Type: audio/wav`，原始 WAV 字节 |
| 探活 | `GET {BASE}/api/v1/health` → 驱动后端指示灯（不调 AI、不耗额度） |
| 证书 | `esp_crt_bundle_attach`（Cloudflare 证书在 IDF 内置 bundle 里） |
| 超时 | **20 秒/次，失败自动重试 3 次**（链路是概率性可用的，见 §4.8） |
| 响应缓冲 | **16KB**（★ 不是 API.md 写的 4KB ★；实测 `TOTAL_BARS=16` 紧凑输出只有 **2769 字节**） |
| 解析 | cJSON，只读 `music` 字段，完全忽略 `labels` |
| 鉴权 | 支持 `Bearer` token 宏；当前后端 `auth_required=false` 所以不带头 |

`?pretty=0` 让后端返回紧凑 JSON，实测体积 2769 字节（约为美化输出的一半）。

### 4.8 实测数据（2026-10，真实调用后端）

| 请求 | 结果 |
|---|---|
| `GET /api/v1/health` | 200，`stage=full`，`auth_required=false`，`ai_binding=true` |
| `POST /api/v1/intent?pretty=0`（2 秒 16kHz WAV） | 200，**耗时 2 秒**，响应 **2769 字节** |
| 返回曲子 | 16 小节 / 48 秒，`chord_notes` 64 个、`melody_notes` 128 个、`bass_notes` 16 个 |
| KS 参数 | `decay=2.0s brightness=0.6 damping=0.38 pick_position=0.4` |
| `intent_source` | `llm`（真的走了模型，不是兜底） |

> ✅ **已解决**：改用自定义域名 `www.230508.xyz` 后，国内直连可用（真机直连实测，
> `GET /health` 200、`POST /intent` 200 / 4 秒 / 2628 字节）。原来的 `*.workers.dev`
> 地址被 DNS 投毒（每次解析到不同的无关海外 IP：Twitter/Dropbox 段），换域名即解决。
>
> 🔒 **额度安全**：`auth_required=false` 意味着任何人拿到这个 URL 都能消耗你的 AI 额度，
> 而子域 `2356766287` 就是邮箱前缀、容易被猜到。
> 建议在 `wrangler.toml` 设 `DEVICE_TOKEN` 并同步填 `BACKEND_DEVICE_TOKEN`。

---

## 5. 与需求/文档的差异记录

### 5.1 实现中发现的「文档 vs 实际后端代码」不一致

以**代码为准**实现，并把 `../esp32-music/docs/API.md` 的三处不一致更正了：

| 项 | API.md 说 | index.js / music.js 实际 |
|---|---|---|
| 响应体积 | 约 2.2KB，缓冲 ≥4KB | `TOTAL_BARS=16` 时紧凑约 2.6KB；32 小节约 **10KB** |
| `music` 字段 | 列了 19 个，无鼓点 | 实际还多出 `bass_notes` / `strum` / `strum_spread_ms` / `damping` / `pick_position` / 鼓点 / `sections` |
| 曲长 | `bars` 1–8 | `TOTAL_BARS` 环境变量控制，clamp 到 **4–32，默认 16**（约 42 秒一首） |

另外发现两个文档没提但有用的东西：`GET /api/v1/health`（已用于后端灯）、
`?pretty=0`（已用于省带宽）。

### 5.2 已知取舍

| 项 | 现状 | 备注 |
|---|---|---|
| 扫弦错开 | 用递减增益近似 22ms 错开 | 真错开需要触发机制带时间偏移；听感差异小，代码复杂度差很多 |
| 鼓点 | ✅ **已实现**（kick / snare / hat） | 配方照搬后端 `demo-page.js:555-621`。**唯一改动**：hat 的高通从 7200Hz 下调到 4500Hz —— 16kHz 采样率的奈奎斯特只有 8kHz，7200Hz 会把能量几乎全滤掉 |
| 编译优化 | `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`（`-Og`） | 只有 KS 组件单独开了 `-O3 -ffast-math`。建议整体切 Release |
| 看门狗 | **已启用**（`CONFIG_ESP_TASK_WDT_EN=1`，5 秒），但**只监控 IDLE 任务** | ★ 本节曾被审计更正 ★ 原文写"未启用，有 30 秒阻塞式 HTTP 会误触发"——**两处都不对**：它本来就是开着的；而且 HTTP 阻塞时会 `vTaskDelay` 让出 CPU，不会饿死 IDLE，所以不会误触发。真正的价值在于把关键任务 `esp_task_wdt_add()` 进去，那样才能发现"任务卡死" |
| 生命灯覆盖范围 | 「调度器活着但某任务卡住」 | 覆盖不了全局死锁，那需要硬件看门狗 |
| `s_last_text` | 只用于日志 | 以后上屏幕/播报时从这里取 |
| KS 音高 | **偏低最多 −78.7 音分**（实测） | 见 `AUDIT.md §4 L1`。属听感调优，等扬声器到货后处理 |

---

## 6. 代码审计（2026-10）

做过一轮 4 通道并行的深度审计，**完整报告见 [`AUDIT.md`](AUDIT.md)**。

### 关键结论

审计发现 **3 条会导致功能失效的严重缺陷**，其中两条是我自己写代码时完全没察觉的：

| 缺陷 | 症状 | 已修 |
|---|---|---|
| **harp 音色全程静音** | 后端 `harp` 的 `pick_position = 0.50` 恰好让梳状滤波把整条激励差分清零。因为 `pick_pos` 是全局参数，**整首曲子一点声音都没有** | ✅ |
| **合成器被跨任务 `memset`** | `KS_Synth_Init` 在按键任务里 `memset` 66KB 合成器对象，而音频任务可能正在渲染它 → 空指针 / 除零崩溃 | ✅ |
| **`xQueueReset` 丢命令** | "停止播放"会清空整个命令队列，连带丢掉刚按下的录音命令 → **上传上一轮的旧音频** | ✅ |

其余修复包括：声部永不回收导致的抢占爆音、`strum` 永远解析成 0（扫弦退化）、
`chord_notes` 越界（可致 ±2MB 偏移崩溃）、LED 可能永久停在错误电平、
录音与打包 WAV 的竞态等。详见 `AUDIT.md`。

### 离线数值验证（不用扬声器也能验）

扬声器未到货，所以把合成器编译到 PC 上做数值断言：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_ks_test.ps1
```

**24 项断言全部通过**，包括「harp 必须出声」（`rms=0.05905`，修复前是 0）、
「声部必须被回收」（曲终后 `= 0`，修复前恒为 32）、
「`strum=1` 与 `strum=0` 输出必须不同」（修复前恒相同）。

### 本次审计的三条经验

1. **审计报告必须独立复核**。子代理报的 2 条结论经我核实是**误报**
   （`BSP_WIFI` 缺失、`KS_MidiToFreq` 无实现），照搬会白改代码。
2. **反过来，我自己漏掉的两条严重缺陷是子代理找到的**（harp 静音、跨任务 memset）——
   所以独立视角有价值，但不能替代复核。
3. **"改完就认为对了"最危险**。本轮有 3 处文档/注释里的错误结论
   （后端"实测可达"、`gain_shift=14`、看门狗"未启用"）都是审计时才发现的，
   而且每一条都曾经让我或读者做出错误判断。

---

## 7. 下一步

1. **硬件到货后**按 `HARDWARE.md §4.5` 的顺序验证：麦克风 → 扬声器 → 后端 → 全链路
2. **完整链路验证**（后端已就绪）：按住 KEY1 说 2~3 秒 → 松手 → 等 `★ music ready`
   → 按 KEY2 循环播放
3. **扬声器到货后调音**（`AUDIT.md §4`）：
   - `damping` 需要按采样率归一（后端数值是 48kHz 调出来的，16kHz 下阻尼强 3~7 倍）
   - 音高偏低的分数延迟补偿方式
   - 用 `tools/run_ks_test.ps1` 做修复前后的对照
4. 可选加固：
   - 整体切 Release 编译
   - 把关键任务 `esp_task_wdt_add()` 进去（看门狗已开，只是没监控业务任务）
   - 把 `s_last_text` 显示到 OLED / 用 TTS 播报
   - 补鼓点合成（`drum_kick` / `drum_snare` / `drum_hat` 已经在 `music_t` 里了）
