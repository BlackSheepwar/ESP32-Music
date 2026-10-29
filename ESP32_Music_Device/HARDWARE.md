# 硬件接线说明（ESP32-S3-N16R8 音乐设备端）

> 本文档是**唯一权威的接线依据**，和代码里的映射表一一对应。
> 改接线时请**同时**改代码：
> - LED / KEY 的引脚 → `User/BSP/BSP_GPIO/bsp_gpio.c` 的 `GPIO_HW_MAP`
> - 麦克风 / 扬声器的引脚 → `User/BSP/BSP_I2S/bsp_i2s.c` 的 `I2S_HW_MAP`

---

## 0. 开发板型号与引脚禁区（★ 先看这一节，看错会浪费一整天 ★）

本工程针对 **ESP32-S3-WROOM-1-N16R8**（16MB flash + 8MB **Octal** PSRAM）。
Octal PSRAM 和 flash 吃掉了大量 GPIO，**下列引脚绝对不能接外设**，
接上去的表现是：上电反复重启 / PSRAM 初始化失败 / flash 读写异常，极难排查。

| 引脚 | 被什么占用 | 能不能接外设 |
|---|---|---|
| GPIO11 ~ GPIO14 | flash D0~D3（SPI0/1） | ❌ **禁止** |
| GPIO16、GPIO17 | flash / PSRAM D4、D5（Octal 扩展） | ❌ **禁止** |
| GPIO26 | PSRAM 片选 CS | ❌ **禁止** |
| GPIO27 ~ GPIO32 | PSRAM DQ1~DQ6 / DQS / CLK | ❌ **禁止** |
| GPIO33 ~ GPIO37 | PSRAM DQ7 及 Octal 数据线 | ❌ **禁止** |
| GPIO43、GPIO44 | UART0 TXD/RXD（串口日志） | 📌 保留 |
| GPIO19、GPIO20 | 原生 USB D-/D+（USB 烧录/CDC） | ⚠️ 慎用 |
| GPIO0 | BOOT 按键，上电低电平进下载模式 | ⚠️ 慎用 |
| GPIO45 | VDD_SPI 选择，上电必须为高 | ⚠️ 见下 |
| GPIO46 | ROM 日志开关，上电最好为低 | ⚠️ 见下 |
| GPIO3 | JTAG 信号源选择，上电最好悬空 | ⚠️ 见下 |

### 本工程怎么绕开 strapping 脚的

| 引脚 | strapping 要求 | 本工程接法 | 为什么满足 |
|---|---|---|---|
| GPIO3 | 悬空即可 | 接 MAX98357 **BCLK（输入脚）** | 复位瞬间引脚为高阻，不驱动 |
| GPIO46 | 上电最好**低** | 接 MAX98357 **LRC（输入脚）** | 复位瞬间高阻 + 内部下拉 → 低 |
| GPIO15~18 | 上电必须**高** | **本工程完全没用这些脚** | ★ 早期版本曾把 LED 放这里，已全部挪走 |

> 补充：`sdkconfig` 里 `CONFIG_BOOT_ROM_LOG_ALWAYS_ON=y`，
> ROM 启动日志被强制打开，不再由 GPIO46 的 strapping 电平决定。
>
> ⚠️ **反例（不要这样接）**：把**按键**接到 GPIO45/GPIO46。
> 按键另一端是 GND，**上电时如果按键被按着**，引脚被拉到 GND，
> 于是 VDD_SPI 掉到 1.8V / ROM 日志被关，直接起不来。
> 本工程 4 个按键刻意避开了这些脚。

---

## 1. 总接线表

### 1.1 指示灯（5 个语义灯 + 1 个预留，高电平点亮）

| 句柄 | 引脚 | 语义 | 表现 |
|---|---|---|---|
| **g_led_hb** | **GPIO4** | **生命灯** | **500ms 方波**（亮500/灭500），由独立任务翻转 |
| **g_led_wifi** | **GPIO5** | **WiFi 状态** | 灭=未开始；快闪=连接中；**常亮=已连上**；慢闪=重连；双闪=长退避 |
| **g_led_mic** | **GPIO6** | **正在录音** | 亮=**KEY1** 正在录音（KEY3 回环录音不点此灯） |
| **g_led_audio** | **GPIO7** | **有可播放音频** | 常亮=有；**载入新音频时快闪三下**；快闪(久)=正在上传处理 |
| **g_led_backend** | **GPIO8** | **后端连通** | 灭=WiFi没连；**快闪=URL没填**（动作项提示）或请求中；**常亮=可达**；慢闪=不可达 |
| g_led_spare | GPIO9 | 预留 | 未使用 |

```
GPIO4 ──[ 470Ω ]──▶|── GND      （5 个灯都一样，LED 长脚接电阻侧）
```

电阻取 **220Ω~1kΩ** 都行；太小会让 GPIO 灌电流超标（推荐 470Ω 或 1kΩ）。

### 1.2 按键（4 个，低电平按下）

| 句柄 | 引脚 | 功能 |
|---|---|---|
| **g_key1** | **GPIO21** | **按住录音 / 松手停止并上传后端** |
| **g_key2** | **GPIO38** | **播放合成音频（循环）/ 再按停止**（下次从头开始） |
| **g_key3** | **GPIO39** | **回环测试：按一下录固定 5 秒到本地**（不上传，自动结束） |
| **g_key4** | **GPIO40** | **播放回环录音（单次）/ 再按停止** |

```
GPIO21 ──┬── 按键 ── GND
         └── （引脚内部已使能上拉，不用外接电阻）
```
松开 = 读到 1（上拉），按下 = 读到 0。代码里 `KEY_ACTIVE_LEVEL = 0` 就是描述这件事。

### 1.3 音频（两个独立 I2S 口）

| 模块 | 模块引脚 | ESP32 引脚 | 说明 |
|---|---|---|---|
| **INMP441**（I2S0，只收） | VDD | 3V3 | ★ 必须并联 100nF + 10uF 到 GND |
| | GND | GND | |
| | **L/R** | **GND** | ★ 接地 = 左声道，必须这样接 |
| | WS | **GPIO42** | 帧时钟 |
| | SCK | **GPIO41** | 位时钟 |
| | SD | **GPIO47** | 数据（ESP32 输入） |
| **MAX98357A**（I2S1，只发） | VIN | 5V（推荐）或 3V3 | ★ 见 §3 |
| | GND | GND | |
| | LRC | **GPIO46** | 帧时钟 |
| | BCLK | **GPIO3** | 位时钟 |
| | DIN | **GPIO2** | 数据（ESP32 输出） |
| | GAIN | 悬空 | 9dB；接 GND=15dB，接 VDD=3dB |
| | **SD** | **接 VIN(5V) 或 3V3** | ★★ **必须显式拉高，不能悬空** ★★ 见下面说明 |
| | SPK+ / SPK- | 扬声器两端 | ★ BTL 差分输出，**两端都不能接地** |

#### ★★ SD 脚为什么必须显式拉高（实测踩过的坑，2026-10）★★

**本节原来写的是「SD 悬空」—— 那条建议是错的，我实测踩了坑。**

| 模块类型 | SD 悬空会怎样 |
|---|---|
| **原厂 MAX98357A** | datasheet 说内部有上拉 → 使能 ✅ |
| **常见廉价仿制模块** | **内部上拉被省略** → 引脚浮空 → **模块关断** ❌ |

**真机实测现象**：

```
SD 悬空时：
    SD   实测 = −0.23V        ← 原厂芯片绝不会是负电压，说明完全没上拉
    SPK+ 实测 = −0.2V         ← 应 ≈VIN/2 = 2.5V
    结果：完全没声音，而软件侧一切正常
          （串口显示 playback finished，数据确实写进了 I2S）

把 SD 接到 VIN 后：
    SPK+ 立刻回到 ≈2.5V  →  出声 ✅
```

**为什么这个坑特别难查**：软件侧完全看不出问题 —— 播放正常结束、DMA 真的在搬数据、
BCLK/WS 用万用表量是正常的 1.65V。**唯一能定位的线索是 SPK± 不等于 VIN/2。**

**正确做法**：用短接线把 `SD` 直接接到 `VIN`（或 `3V3`）。
两者都远高于使能阈值（>1.2V），**在任何模块版本上都正确**。

> **一般性教训**：凡是"依赖器件内部默认值"的接法都有风险 —— 换一块板子、
> 引脚氧化、受潮都可能失效。**显式连接才可靠**，生产环境尤其如此。

---

## 2. 各指示灯与状态的对应关系（调试时对照这张表）

| 你看到的现象 | 含义 | 该怎么办 |
|---|---|---|
| 生命灯不闪 | 调度器出问题或系统挂了 | 看串口是否反复重启 |
| 生命灯闪，其它灯全灭 | 系统活着，WiFi 还没开始/没配置 | 检查 `WIFI_SSID` 宏 |
| WiFi 灯快闪不停 | 正在连 / 连不上 | 看串口 reason：201=找不到AP，15/205=密码错 |
| **WiFi 灯常亮** | ★ WiFi OK，拿到 IP 了 | |
| **后端灯快闪** | ★ **`BACKEND_BASE_URL` 还没填**（这是一个动作项，去改配置） | 见 §3.2 |
| 后端灯慢闪 | URL 填了，但后端到不了 | 换手机热点试一下，区分网络环境 / 固件问题，见 §3.2 |
| 后端灯常亮 | 全链路通 | 可以按 KEY1 试录音上传 |
| 后端灯灭 | WiFi 都没连上，后端无从谈起 | 先看 WiFi 灯 |
| 音频灯常亮 | 内存里有可播放的合成音乐 | 按 KEY2 播放 |
| 音频灯闪三下 | 刚载入了一段新音乐 | 按 KEY2 播放 |
| 音频灯快闪很久 | 正在上传 + 等 AI 推理（3~10 秒） | 等着 |
| 录音灯亮 | 正在录音 | KEY1 松手结束，或等录满自动停 |

---

## 3. 后端对接（★ 你自己的问题：「写个网址就能通吗」★）

### 3.1 答案：是，只需要填一个完整地址

**只需要填一个宏**（`User/APP/APP_MUSIC/app_music_cfg.h`）：

```c
#define BACKEND_BASE_URL        "https://music-intent-api.<你的子域>.workers.dev"
```

代码自动拼成两个调用：

| 用途 | 请求 |
|---|---|
| 上传音频要音乐 | `POST {BASE}/api/v1/intent?pretty=0`，Content-Type: audio/wav |
| 探活（驱动后端灯） | `GET {BASE}/api/v1/health` |

固定要做的三件事代码里已经写好了：

| 项 | 值 | 为什么 |
|---|---|---|
| 请求体 | **原始 WAV 字节** | 后端读 `request.arrayBuffer()`，不是 base64/multipart |
| 证书 | `esp_crt_bundle_attach` | Cloudflare 证书在 IDF 内置 bundle 里，不用手动塞 |
| 超时 | **30 秒** | 默认 5 秒必然超时，而云端日志显示成功，两边矛盾极难查 |

`Authorization` 头现在**不需要**：后端 `wrangler.toml` 里 `DEVICE_TOKEN = ""`，空值直接放行。
以后开鉴权就填 `BACKEND_DEVICE_TOKEN` 宏。

### 3.2 ✅ 已解决：改用自定义域名，国内直连可用

**最终方案**：买了一个域名 `230508.xyz`，托管到 Cloudflare，
把 Worker 绑到 `www` 主机名上，**国内网络直连可用，不需要任何代理**。

```
#define BACKEND_BASE_URL   "https://www.230508.xyz"     ← 固件里已是这个
```

#### 实测数据（2026-10，全部为【真机直连、不走代理】验证）

| 请求 | 结果 |
|---|---|
| `GET  https://www.230508.xyz/api/v1/health` | **HTTP 200**，`stage=full`，`auth_required=false`，262 字节 |
| `POST https://www.230508.xyz/api/v1/intent?pretty=0`（2 秒 WAV） | **HTTP 200，耗时 4 秒，响应 2628 字节**，返回 16 小节 / 48 秒完整曲子 |

DNS 与连通性：

| 项 | 值 |
|---|---|
| NS | `raphaela.ns.cloudflare.com` / `tadeo.ns.cloudflare.com` |
| `www.230508.xyz` | `172.67.183.199` / `104.21.48.97`（真 Cloudflare 段） |
| TCP 443 | 两个 IP **都通** |

> ★ **验证方法很重要**：以上用的是 `.NET HttpWebRequest + Proxy = null`
> 和原生 socket，是真正不走代理的直连测试。
> **不能**用 PowerShell 的 `-Proxy $null` —— 它的语义是"使用 IE 默认设置"，
> 仍然会走系统代理（本机是 `127.0.0.1:7897`），会得出假阳性结论。

#### 为什么必须换掉原来的 `workers.dev` 地址

原地址 `music-intent-api.2356766287.workers.dev` 在**国内不可用**，证据链：

| 测试 | 结果 |
|---|---|
| 反复解析该域名 | 每次拿到**不同的、毫不相干的海外 IP**：`103.240.180.117`、`104.244.46.211`(**Twitter**)、`118.193.202.219`、`108.160.163.106`(**Dropbox**) |
| 对上述 IP 测 TCP 443 | **全部不可连**（设备端 `errno=113 EHOSTUNREACH`，约 18 秒超时） |
| 同网络访问 `cloudflare.com` / `developers.cloudflare.com` | `104.16.x.x` **TCP 和 HTTPS 都通** |

→ **被针对的是 `workers.dev` 这个域名本身，不是 Cloudflare 的 IP 段。**
所以换成自定义域名立刻就好了。

> ⚠️ **域名别打错**：一开始买的时候一直是 `2230508.xyz`（多了一个 `2`），
> 而实际注册的是 **`230508.xyz`**。注册局的三个权威 NS 和 RDAP 都返回
> "域名不存在"，排查了很久才发现是拼写问题。
> 教训：域名不通时**先核对拼写**，再用 RDAP 查注册状态
> （`https://rdap.centralnic.com/xyz/domain/你的域名`，404 = 未注册）。

#### 本文档的三次判断更正（留档，避免重犯）

| 阶段 | 当时的说法 | 对错 |
|---|---|---|
| 最初 | 「`workers.dev` 被墙，必须买域名」 | 方向对，但只是**推测** |
| 中间 | 「实测可达，不用买域名」 | ❌ **错**：那次"实测"走了系统代理，不是直连 |
| 后来 | 「国内用 Cloudflare 自定义域名不通是常态」 | ❌ **也错**：`www.230508.xyz` 实测直连正常 |
| 最终 | 「换自定义域名后直连可用」 | ✅ 有真机直连证据 |

#### ⚠️ 重要：这条链路是【概率性】可用的，不是稳定可用

设备实测（2026-10，连 `www.230508.xyz`）：

| 探活次数 | 结果 | 耗时 |
|---|---|---|
| #1 | ✅ HTTP 200，证书验证通过 | 3919 ms |
| #2 | ✅ HTTP 200 | 3601 ms |
| #3 | ✅ HTTP 200 | 8073 ms |
| #4 | ❌ `mbedtls_ssl_handshake returned -0x7280` | 15684 ms |

`-0x7280` = **`MBEDTLS_ERR_SSL_TIMEOUT`** —— 注意不是证书错误（日志里证书是验证通过的），
而是**握手包发出去了、服务端的响应被丢掉了**。

**这是 GFW 对 Cloudflare 的典型形态：概率性干扰，不是确定性封锁。**

判断依据（三条互相印证）：
1. 成功的那几次要 3.6~8 秒，而 PC 直连只要 **0.3 秒** —— 慢了一个数量级
2. 失败是"发出去没回音"（timeout），不是"连接被拒绝"
3. 时好时坏 —— 概率性，不是稳定的通或不通

> ★ 所以「电脑能打开网页」不代表设备能稳定工作 ——
> 浏览器会自动重试很多次，而固件原本只试一次。

#### 固件的应对：自动重试

`bsp_http.h` 里加了重试策略，对**探活和上传都生效**：

| 宏 | 值 | 说明 |
|---|---|---|
| `BSP_HTTP_MAX_RETRY` | 3 | 最多尝试 3 次（含第一次） |
| `BSP_HTTP_RETRY_BASE_MS` | 800 | 重试前等待 800ms / 1600ms（递增） |
| `BSP_HTTP_DEFAULT_TIMEOUT_MS` | 20000 | 单次超时（从 30 秒下调，因为握手失败时干等没意义） |

设计要点：
- **只在网络层失败时重试**（`CONNECT` / `TIMEOUT`）；
  后端的 4xx/5xx 是明确答复，不重试
- 失败形态是 TLS 握手，**重试不会重复上传音频**（此时请求体根本没发出去）
- 退避递增，因为干扰是成片的，隔一会出现空窗期才能穿过去

实测效果：原本约 75% 成功率，重试后接近 100%。

> 串口上会看到这样的重试日志：
> ```
> W BSP_HTTP: 第 1/3 次尝试失败(CONNECT_FAIL(...))，800 ms 后重试...
> I BSP_HTTP: 重试第 1 次成功
> ```
>
> ⚠️ 如果持续重试 3 次都失败，那说明遇上了较长的干扰窗口，
> 等一会儿会自动恢复（探活每 30 秒一次）。

#### 想让链路更稳的长期选择

| 方案 | 成本 | 稳定性 |
|---|---|---|
| 现状（Cloudflare 自定义域名 + 重试） | 已投入 | 约 75%→99%，偶发失败 |
| 换国内 Serverless（阿里云 FC / 腾讯云 SCF） | 域名要 ICP 备案 + 换 AI 服务 | 国内直连，最稳 |
| 上游路由器挂代理 | 需要软路由 | 稳定，但依赖代理 |
#### 固件里备好的排查工具（现在用不上了，但留着）

- **DNS 覆盖开关**（`BACKEND_DNS_OVERRIDE_ENABLE`）：换 DNS 服务器试，
  用来区分「DNS 污染」和「TCP 到不了」。当前默认 DNS 就够用，默认关闭。
- **失败时自动分层诊断**（`BACKEND_DIAG_ON_FAIL`，默认开）：后端探活失败时依次测
  DNS 解析 → TCP 连接，给出明确结论：
  - `DNS 解析失败` → 换 DNS 可能解决
  - `DNS 给出 IP 但 TCP 连不通` → IP 被污染或路径被掐
  - `TCP 通` → 问题在 TLS 层

> 🔒 **额度安全提醒**：后端 `auth_required=false`，**任何人拿到这个 URL
> 都能调用并消耗你的 Workers AI 额度**。而且域名已经写死在固件里。
> 要保护额度：在 `esp32-music/worker/wrangler.toml` 设 `DEVICE_TOKEN = "一串随机字符"`，
> 再把同样的值填到 `app_music_cfg.h` 的 `BACKEND_DEVICE_TOKEN`，重新部署 + 烧录。
#### 固件里已备好的排查工具

- **DNS 覆盖开关**（`app_music_cfg.h` 的 `BACKEND_DNS_OVERRIDE_ENABLE`）：
  换一个 DNS 服务器试，用来区分「DNS 污染」和「TCP 到不了」。
  连上后串口会打印实际使用的 DNS，方便确认覆盖有没有生效。
- **失败时自动分层诊断**（`BACKEND_DIAG_ON_FAIL`，默认开）：后端探活失败时依次测
  DNS 解析 → TCP 连接，并给出明确结论：
  - `DNS 解析失败` → 换 DNS 可能解决
  - `DNS 给出 IP 但 TCP 连不通` → IP 被污染或路径被掐（**你现在就是这个**）
  - `TCP 通` → 问题在 TLS 层

> 🔒 **额度安全提醒**：后端 `auth_required=false` 意味着**任何人拿到这个 URL
> 都能调用并消耗你的 Workers AI 额度**（而子域 `2356766287` 就是邮箱前缀，很好猜）。
> 要保护额度：在 `esp32-music/worker/wrangler.toml` 里设 `DEVICE_TOKEN = "一串随机字符"`，
> 再把同样的值填到 `app_music_cfg.h` 的 `BACKEND_DEVICE_TOKEN`，然后重新部署 + 烧录。

如果以后想换成自己的域名（子域在哪看，三选一）：
1. Cloudflare 控制台 → Workers & Pages → 右侧 **Your subdomain**
2. 在 `esp32-music/worker` 目录跑 `npx wrangler deploy`，部署成功会打印完整地址
3. Cloudflare 控制台 → 点进 `music-intent-api` → Settings

---

## 4. 烧录与验证步骤

### 4.1 配置（两个宏）

```c
// User/APP/APP_WIFI/app_wifi_cfg.h    ← WiFi
#define WIFI_SSID               "你的2.4G WiFi名字"
#define WIFI_PASSWORD           "你的WiFi密码"

// User/APP/APP_MUSIC/app_music_cfg.h  ← 后端
#define BACKEND_BASE_URL        "https://music-intent-api.<你的子域>.workers.dev"
```

> ★ WiFi 只支持 **2.4GHz**。路由器双频合一时建议把 2.4G 的 SSID 单独分出来。
> 忘了改的话代码会检测 `YOUR_` 前缀并在串口给提示，对应指示灯也不会傻等。

### 4.2 编译与烧录

```powershell
cd D:\a1\Project\Study\ESP32_Music_Device
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor

# 如果 idf.py 报 PermissionError: [WinError 5]（命名管道被禁），用附带的脚本：
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

### 4.3 上电预期现象（按顺序）

| 时间 | 现象 |
|---|---|
| 上电 | 生命灯立刻开始 **500ms 方波** |
| ~100ms | WiFi 灯**快闪** |
| 连上后 1~3 秒 | **WiFi 灯常亮**，串口打印 `★ CONNECTED ★ ip=... rssi=...` |
| 连上后 ~0.5 秒 | 后端灯**快闪（请求中）→ 常亮**（实测探活成功，2 秒内返回 200） |
| 之后 | 每 10 秒打印 `alive: ip=... rssi=...`；每 30 秒一次后端探活 |

### 4.4 硬件没到货时怎么验

**麦克风、扬声器都还没到货，也能验证大部分逻辑**：

| 操作 | 预期（无麦克风/扬声器时） |
|---|---|
| 上电 | 生命灯闪、WiFi 灯亮、后端灯慢闪 |
| 按 KEY3 | 录音灯亮 5 秒后自动灭；串口打印 `loopback record done: N frames` |
| | ★ 如果 N = 0，说明麦克风没数据（正常，因为没接） |
| 按 KEY4 | 串口打印 `还没有回环录音` 提示灯闪两下（因为 KEY3 录到 0 帧） |
| 按 KEY1 长按 2 秒松手 | 串口打印 `录音不足 0.5 秒，取消上传`（因为录不到数据） |
| 按 KEY2 | 串口打印 `没有可播放音频`，音频灯闪两下 |

**要验 KS 合成器有没有声**（不需要后端、不需要麦克风）：
临时在 `main.c` 的 `App_MusicApp_Init()` 之后加一段「用假 music_t 灌进合成器播放」的测试，
或者等扬声器到货后按下面 §4.5 走完整链路。

### 4.5 硬件到货后的完整验证顺序

1. **先验麦克风**：按 KEY3 录 5 秒，看串口 `loopback record done: 80000 frames (5.0 s)`
   - `frames = 0` → SD/SCK/WS 接线或 L/R 脚问题
2. **再验扬声器**：按 KEY4 回放刚才录的
   - **完全没声** → 先测 **SPK+ 对 GND 的电压**（播放中）：
     - **≈VIN/2（5V 供电时 ≈2.5V）** = 功放工作正常，问题在信号或喇叭
     - **≈0V 或一直不变** = 功放没工作 → **查 SD 脚有没有显式接到 VIN**
       （见 §1.3 的说明，悬空在仿制模块上会导致关断，这是实测踩过的坑）
   - 有输出偏置但仍无声音 → 查 VIN 供电、SPK+/SPK- 是否接对（★ 两端都不能接地）
3. **再验后端**：挂上域名让后端灯变常亮
4. **最后验完整链路**：KEY1 按住说一句话 → 松手 → 等 3~10 秒 →
   音频灯闪三下 → KEY2 播放

### 4.6 串口日志关键字速查

| 日志 | 含义 |
|---|---|
| `[SYS] PSRAM size : 8 MB (initialized)` | PSRAM 正常 |
| `[SYS] PSRAM size : 0 MB (NOT initialized!)` | ⚠️ PSRAM 没起来，录音缓冲和合成器会分配失败 |
| `[APP_MUSIC] BACKEND_BASE_URL is still the placeholder!` | 没填后端地址 |
| `[APP_MUSIC] init done: resp buf 16384 B, music ... synth ... KB` | 大内存分配成功 |
| `[KS] init: fs=16000 Hz, decay=1.90s -> d=..., damping=..., pick=...` | 合成器参数换算成功 |
| `[BSP_HTTP] POST ... (bytes upload)` | 开始上传 |
| `[BSP_HTTP] HTTP 200 OK, body N bytes` | 上传成功 |
| `[BSP_HTTP] CONNECT_FAIL(网络不可达/被墙/DNS)` | DNS 失败或网络不可达（换热点试） |
| `[BSP_HTTP] HTTP 400 ... body: ...` | 后端拒绝，body 里有 `hint` 直接告诉你改什么 |
| `[APP_MUSIC] intent_source = fallback (⚠️ 走了后端兜底)` | 后端 LLM 失败，音乐不会随说话内容变化 |

---

## 5. 附录：idf.py 起不来时的替代方案

`idf.py` 是 Python 程序，用 asyncio 捕获子进程输出，
而 **Windows 上 asyncio 是用「命名管道」实现的**。
某些受限/沙箱环境禁止创建命名管道，于是 `idf.py` 会在
`create_subprocess_exec` 处直接抛 `PermissionError: [WinError 5]`。

`build.ps1` 绕开了这一层：直接按 `idf.py` 内部的顺序调用 **CMake + Ninja**，
产物与 `idf.py build` 完全一致。

```powershell
# Windows PowerShell 默认禁止运行未签名脚本，所以要加 -ExecutionPolicy Bypass
powershell -ExecutionPolicy Bypass -File .\build.ps1
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean   # 全量重编
```

> 注：`build.ps1` 刻意**只用 ASCII 字符**（注释用英文）。
> 因为 Windows PowerShell 5.1 在文件没有 UTF-8 BOM 时会按 ANSI(GBK) 解码 `.ps1`，
> 中文注释会变乱码，严重时直接破坏脚本语法。
