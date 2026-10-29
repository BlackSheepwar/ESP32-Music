/**
 * @file app_music_cfg.h
 * @brief 后端对接配置（★ 要改后端地址就改这里 ★）
 * @author 李嘉图
 * @date 2026-06-26
 *
 * @note 本文件是设备端与云端之间的唯一配置点。
 */

#ifndef __APP_MUSIC_CFG_H__
#define __APP_MUSIC_CFG_H__

/*==============================================================================
 * ★★★ 后端地址（已配好，一般不用改）★★★
 *============================================================================*/
/**
 * @brief 后端基址（只填到域名，不要带路径、不要带结尾斜杠）
 *
 *   代码会自动拼成：
 *       POST {BACKEND_BASE_URL}/api/v1/intent?pretty=0     ← 上传音频要音乐
 *       GET  {BACKEND_BASE_URL}/api/v1/health              ← 探活，用于后端指示灯
 *
 *   ★★★ 实测结论（2026-10，真实设备 + 真机直连验证）★★★
 *
 *   ✅ 当前用的是【自定义域名】，国内网络直连可用，不需要任何代理：
 *
 *       https://www.230508.xyz/api/v1/health
 *         → HTTP 200，stage=full，auth_required=false，响应 262 字节
 *       https://www.230508.xyz/api/v1/intent?pretty=0   （2 秒 WAV）
 *         → HTTP 200，耗时 4 秒，响应 2628 字节，返回 16 小节 / 48 秒的完整曲子
 *
 *       ★ 验证方式很重要：以上用的是原生 HttpWebRequest + Proxy=null，
 *         也就是【真正不走代理】的直连测试。不是在被代理转发的情况下测的。
 *
 *   ⛔ 为什么不用原来的 *.workers.dev 地址：国内不可用。证据链（都实测过）：
 *        · DNS 被投毒 —— 同一域名反复解析，拿到各种毫不相干的海外 IP：
 *            103.240.180.117 / 104.244.46.211 (Twitter) /
 *            118.193.202.219 / 108.160.163.106 (Dropbox)
 *        · 对上述 IP 测 TCP 443，全部不可连（errno=113 EHOSTUNREACH）
 *        · 而同一网络下访问 cloudflare.com / developers.cloudflare.com 的
 *            104.16.x.x 真 Cloudflare 地址，TCP 与 HTTPS 全都通
 *        → 结论：被针对的是 workers.dev 这个域名本身，不是 Cloudflare 的 IP 段。
 *          所以换成自定义域名就通了 —— 这正是现在用的方案。
 *
 *   ★ 记录两次判断失误，避免以后重犯 ★
 *     1) 最初说「需要买域名」方向是对的，但当时只是推测，没有证据。
 *     2) 中间一度说「实测可达、不用买域名」是【错的】：
 *        那次用了 PowerShell 的 -Proxy $null，而它的语义是
 *        "使用 IE 默认设置"，实际仍然走了系统代理 127.0.0.1:7897。
 *        ★ 教训：验证「国内能否直连」必须用 .NET HttpWebRequest + Proxy=null，
 *          或用原生 socket；PowerShell 的 -Proxy 参数不可信。★
 *
 *   ★ 换域名时怎么改 ★
 *     1) Cloudflare 控制台 → Workers & Pages → music-intent-api
 *        → Settings → Domains & Routes → Add → ★ Custom Domain ★（不要选 Route）
 *     2) 只改下面这一行宏，重新烧录。固件其它地方一行都不用动。
 *
 *   ⚠️ DEVICE_TOKEN 安全提醒：
 *      当前后端 auth_required=false，也就是【任何人拿到这个 URL 都能调用，
 *      并消耗你的 Workers AI 额度】。而 URL 已经写死在固件里。要保护额度：
 *        1) 在 esp32-music/worker/wrangler.toml 设 DEVICE_TOKEN = "一串随机字符"
 *        2) 把同样的值填到下面的 BACKEND_DEVICE_TOKEN
 *        3) 重新部署 Worker + 重新烧录固件
 */
#define BACKEND_BASE_URL        "https://www.230508.xyz"
/*==============================================================================
 * DNS 配置（★ 用来验证"是不是 DNS 被污染"★）
 *============================================================================*/
/**
 * @brief 自定义 DNS 服务器（0 = 用 DHCP 下发的）
 * @note  ★ 为什么需要这个开关 ★
 *        ESP32 默认用路由器 DHCP 下发的 DNS。如果那个 DNS 把这个域名
 *        解析到错误的 IP，TCP 连接就会以 EHOSTUNREACH 立刻失败 ——
 *        现象和"服务器挂了"一模一样，极难区分。
 *        填一个不同的 DNS（比如 223.5.5.5 阿里 / 119.29.29.29 腾讯）
 *        就能把「DNS 污染」和「真的连不上」区分开。
 *
 *        ⚠️ 实测提醒：本工程【当前已经不需要这个开关】——
 *          自定义域名 www.230508.xyz 国内直连正常，默认 DHCP 的 DNS 就够用。
 *          留着它是为了以后换网络（比如某些校园网/企业网的 DNS 会解析错）时
 *          能快速排除 DNS 因素。当时查 workers.dev 那个地址时，
 *          四个公共 DNS 给出的 IP 全部 TCP 443 不可达，
 *          换了也没用 —— 那种情况是域名被针对，不是 DNS 的问题。
 */
#define BACKEND_DNS_OVERRIDE_ENABLE     0

/** @brief 主 DNS（BACKEND_DNS_OVERRIDE_ENABLE = 1 时生效） */
#define BACKEND_DNS_PRIMARY             "223.5.5.5"

/** @brief 备用 DNS（可留空串） */
#define BACKEND_DNS_SECONDARY           "119.29.29.29"

/*==============================================================================
 * 网络诊断
 *============================================================================*/
/**
 * @brief 每次探测后端失败时，是否额外做一轮网络连通性诊断
 * @note  1 = 开（推荐）：失败时依次测
 *          ① 网关是否可达（ICMP ping，不通则说明根本没连上局域网）
 *          ② 公共 IP 是否可达（ping 223.5.5.5，不通说明没上外网）
 *          ③ 对后端域名做 TCP 连接测试（区分 DNS 问题 / TCP 被掐）
 *        这三步能把"连不上"拆成三个明确的层次，省掉反复猜。
 *        代价：失败时多花约 3~10 秒，且只在失败时发生。
 */
#define BACKEND_DIAG_ON_FAIL            1

/**
 * @brief 设备令牌（后端 DEVICE_TOKEN）
 * @note  当前后端 wrangler.toml 里 DEVICE_TOKEN = ""，即【不校验】，
 *        所以这里留空即可，请求不会带 Authorization 头。
 *        以后要开鉴权：在后端把 DEVICE_TOKEN 设上，再把这个宏填成同样的值。
 */
#define BACKEND_DEVICE_TOKEN    ""

/*==============================================================================
 * 探活（后端指示灯用）
 *============================================================================*/
/**
 * @brief 是否启用后端探活
 * @note  1 = 启用：上电连上 WiFi 后，每 BACKEND_PROBE_PERIOD_MS 请求一次
 *            /api/v1/health，据此驱动「后端」指示灯。
 *        0 = 关闭：后端指示灯只在真正上传音频时更新。
 *
 *        ★ 启用的好处：能一眼区分「WiFi 没连上」和「WiFi 连上了但后端到不了」。
 *          在排查 workers.dev 那个地址时（国内不可用），这个区分非常有用：
 *          WiFi 灯常亮 + 后端灯慢闪 = 网络通、云端到不了，不是固件问题。
 *
 *        ★ 探活的代价很小：health 接口不调 AI、不耗额度、响应只有几百字节。
 *
 *        ★ URL 没填时后端灯会【快闪】提示 ★（不是恒灭）
 *          恒灭跟"没有这个灯"视觉上一样，会让人以为灯坏了或固件没跑。
 *          改成快闪后，它是一个明确的动作项：「该去填 BACKEND_BASE_URL 了」。
 *          填好之后变：慢闪=连不上 / 常亮=全链路通（本工程当前地址实测是通的）。
 */
#define BACKEND_PROBE_ENABLE    1

/** @brief 探活周期（毫秒）。30 秒足够及时，也不会太频繁 */
#define BACKEND_PROBE_PERIOD_MS 30000

/*==============================================================================
 * 上传与播放行为
 *============================================================================*/
/**
 * @brief 录音最大时长（秒）
 * @note  后端 MAX_AUDIO_SECONDS 默认 15 秒，超过会被 413 拒绝。
 *        这里取 8 秒：既够说一句话，又留了充足余量，
 *        而且 8 秒 @16kHz 单声道 16bit = 256KB，放 PSRAM 毫无压力。
 *        ★ 前端到端预算见 API.md §11：上传 160KB 约 0.5~1.5 秒。
 */
#define RECORD_MAX_SECONDS      8

/**
 * @brief 回环测试录音时长（秒）
 * @note  按需求：KEY3 按一下录固定长度，不按第二下。
 */
#define LOOPBACK_RECORD_SECONDS 5

/**
 * @brief 播放完成后是否自动循环（KEY2 播合成音频）
 * @note  按需求：KEY2 是循环播放，再按停止后下次从头开始。
 */
#define PLAYBACK_LOOP_ENABLE    1

/*==============================================================================
 * 调试开关
 *============================================================================*/
/** @brief 是否把后端返回的 text / intent_source 等诊断字段打到串口 */
#define MUSIC_LOG_RESPONSE      1

/**
 * @brief 后端响应缓冲大小（字节）
 * @note  ★ 不要按 API.md 写的 4KB ★
 *        实测：TOTAL_BARS=16 时美化输出约 6.2KB、紧凑输出约 2.6KB；
 *        TOTAL_BARS 拉到 32 时约 10KB。所以用 16KB 才有余量。
 *        配合 ?pretty=0 使用，正常情况下只用到 3KB 左右。
 */
#define MUSIC_RESP_BUF_SIZE     16384

#endif /* __APP_MUSIC_CFG_H__ */
