/**
 * ESP32 智能音乐生成 —— 云端意图解析 API
 * ===========================================================================
 *
 * 职责边界（很重要，写错了会踩 10ms CPU 上限）：
 *
 *   本 Worker 只做三件事：接收音频 -> 调用 AI -> 展开乐理并返回 JSON。
 *   绝对不做音频处理（解码 / 重采样 / FFT / DSP）。
 *
 *   原因：Cloudflare Workers 免费版每请求只有 10ms CPU 时间。
 *   - 等待 fetch()、AI 推理等网络 I/O **不计入** CPU 时间
 *   - 任何纯计算（包括 base64 编码）**计入** CPU 时间
 *   所以把音频原样转发出去是安全的，在 Worker 里解音频必死。
 *   参考: https://developers.cloudflare.com/workers/platform/limits/#cpu-time
 *
 * 四阶段设计（用 wrangler.toml 里的 STAGE 环境变量切换）：
 *   mock  —— 不调 AI，返回固定结构化数据。硬件没到货时用它开发 ESP32 端。
 *   echo  —— 解析 WAV 头 + 音量，给出诊断建议。用来分离「硬件问题」和「云端问题」。
 *   asr   —— 只跑 Whisper。
 *   full  —— Whisper + LLM + 乐理展开。
 *
 *   这个分阶段不是为了好看：调试时最大的时间浪费是「不知道错在哪一层」。
 *   有了阶段，你永远只在一个变量上做实验。
 */

import { expandMusicIntent } from './music.js';
import { DEMO_PAGE } from './demo-page.js';

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/**
 * 最终形态使用的语音识别模型。turbo 版本质量明显好于 @cf/openai/whisper，价格接近。
 *
 * 可以通过环境变量 ASR_MODEL / LLM_MODEL 覆盖，用来在不改代码的情况下换模型排查问题。
 * 排查时非常有用：如果怀疑是某个模型的问题，改 wrangler.toml 就能验证。
 */
const DEFAULT_ASR_MODEL = '@cf/openai/whisper-large-v3-turbo';
const DEFAULT_LLM_MODEL = '@cf/meta/llama-3.1-8b-instruct-fp8-fast';

const asrModel = (env) => String(env.ASR_MODEL ?? '').trim() || DEFAULT_ASR_MODEL;
const llmModel = (env) => String(env.LLM_MODEL ?? '').trim() || DEFAULT_LLM_MODEL;

/** mock 阶段返回的固定意图：一段华语流行风格的 I-vi-ii-V */
const MOCK_INTENT = {
  style: 'mandopop',
  instrument: 'nylon_guitar',
  bpm: 92,
  key: 'C',
  scale: 'major',
  bars: 4,
  chords: ['Cmaj7', 'Am7', 'Dm7', 'G7'],
  arp: 'fingerpick',
};

const CORS_HEADERS = {
  'Access-Control-Allow-Origin': '*',
  'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  'Access-Control-Allow-Headers': 'Content-Type, Authorization',
  'Access-Control-Max-Age': '86400',
};

const INTENT_SYSTEM_PROMPT = `你是一个音乐制作助手。用户会用自然语言描述想听的音乐，你要把它翻译成结构化的编曲参数。

只输出一个 JSON 对象。不要输出任何解释文字，不要用 markdown 代码块包裹。

JSON 字段定义：
- "style": 风格标签，例如 "mandopop" / "rock" / "jazz" / "ballad" / "edm" / "folk" / "rnb" / "citypop"
- "instrument": 音色，必须是以下之一：nylon_guitar, steel_guitar, clean_electric, harp, banjo, bass_pluck
- "bpm": 整数，40 到 220 之间
- "key": 调式主音，必须是 "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" 之一
- "scale": 必须是 "major" 或 "minor" 之一
- "bars": 整数，2 到 8 之间
- "chords": 和弦符号数组，长度必须等于 bars。用标准写法，例如 "Cmaj7"、"Am7"、"Dm7"、"G7"、"F"、"G"
- "arp": 伴奏音型，必须是以下之一：arp_up, arp_updown, arp_wave, arp_16th, fingerpick, travis, strum, bass_only

编曲要求：
- 用户提到某位歌手或乐队时，采用那个风格典型的和弦走向与速度
- chords 里的和弦必须都是 key 和 scale 的调内和弦
- 抒情风格用较慢 bpm（60-90）和 fingerpick；节奏感强的用较快 bpm（110-140）和 arp_wave 或 strum
- 只输出 JSON，不要有其他内容`;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

function jsonResponse(body, status = 200) {
  return new Response(JSON.stringify(body, null, 2), {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      // ★ 禁止任何环节缓存 API 响应。
      //   实测踩过坑：改了页面重新部署后，通过代理访问拿到的还是旧版本，
      //   表现为"明明部署成功了但值没变"，非常误导。
      'Cache-Control': 'no-store, must-revalidate',
      ...CORS_HEADERS,
    },
  });
}

/**
 * Uint8Array -> base64
 *
 * 必须分块：String.fromCharCode.apply 的参数个数有上限（约 65535），
 * 一次性展开 16 万字节会直接抛 "Maximum call stack size exceeded"。
 * 这是把二进制喂给 Whisper 的标准做法。
 */
function bytesToBase64(bytes) {
  const CHUNK = 0x8000; // 32768
  let binary = '';
  for (let i = 0; i < bytes.length; i += CHUNK) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return btoa(binary);
}

/** 从 LLM 输出里尽量把 JSON 抠出来（容忍 markdown 围栏和前后废话） */
function extractJson(text) {
  const s = String(text ?? '');
  const fenced = /```(?:json)?\s*([\s\S]*?)```/i.exec(s);
  const body = fenced ? fenced[1] : s;
  const start = body.indexOf('{');
  const end = body.lastIndexOf('}');
  if (start < 0 || end <= start) return null;
  try {
    return JSON.parse(body.slice(start, end + 1));
  } catch {
    return null;
  }
}

// ---------------------------------------------------------------------------
// WAV 分析
// 注意：这里只做「读头部 + 抽样统计」，不是音频解码，CPU 开销可忽略。
// ---------------------------------------------------------------------------

function parseWav(buffer) {
  if (buffer.byteLength < 44) {
    return { ok: false, reason: `body too short: ${buffer.byteLength} bytes (WAV header alone is 44)` };
  }

  const dv = new DataView(buffer);
  const tag = (o) =>
    String.fromCharCode(dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2), dv.getUint8(o + 3));

  if (tag(0) !== 'RIFF') return { ok: false, reason: 'missing RIFF magic (前 4 字节应该是 "RIFF")' };
  if (tag(8) !== 'WAVE') return { ok: false, reason: 'missing WAVE magic (第 9-12 字节应该是 "WAVE")' };

  // 遍历 chunk，别假设 data 一定紧跟在 44 字节之后
  // （设备端如果加了 LIST/fact 之类的块，硬编码偏移就会读错）
  let fmt = null;
  let dataOffset = -1;
  let dataSize = 0;

  let off = 12;
  while (off + 8 <= buffer.byteLength) {
    const id = tag(off);
    const size = dv.getUint32(off + 4, true);
    const body = off + 8;

    if (id === 'fmt ') {
      fmt = {
        audioFormat: dv.getUint16(body, true),
        channels: dv.getUint16(body + 2, true),
        sampleRate: dv.getUint32(body + 4, true),
        byteRate: dv.getUint32(body + 8, true),
        bitsPerSample: dv.getUint16(body + 14, true),
      };
    } else if (id === 'data') {
      dataOffset = body;
      dataSize = Math.min(size, Math.max(0, buffer.byteLength - body));
      break;
    }
    off = body + size + (size % 2); // chunk 按偶数字节对齐
  }

  if (!fmt) return { ok: false, reason: 'no fmt chunk found' };
  if (dataOffset < 0) return { ok: false, reason: 'no data chunk found' };
  if (fmt.audioFormat !== 1) {
    return { ok: false, reason: `not uncompressed PCM (audioFormat=${fmt.audioFormat}, 1=PCM)` };
  }
  if (fmt.bitsPerSample !== 16) {
    return { ok: false, reason: `expected 16-bit PCM, got ${fmt.bitsPerSample}-bit` };
  }

  const durationSec = fmt.byteRate > 0 ? dataSize / fmt.byteRate : 0;
  const stats = pcmStats(dv, dataOffset, dataSize, fmt.channels);

  return {
    ok: true,
    ...fmt,
    durationSec: +durationSec.toFixed(2),
    dataBytes: dataSize,
    ...stats,
  };
}

function pcmStats(dv, offset, dataSize, channels) {
  const frameBytes = 2 * channels;
  const frames = Math.floor(dataSize / frameBytes);
  if (frames <= 0) return { rms: 0, peak: 0, dc: 0, clipped: 0, frames: 0, sampled: 0 };

  // 抽样扫描：16 万采样点全扫约 1-2ms，虽然仍在 10ms 预算内，
  // 但没必要为一个诊断字段花掉 CPU。8000 个样本统计上完全够用。
  const MAX_SCAN = 8000;
  const stride = Math.max(1, Math.floor(frames / MAX_SCAN));

  let sumSq = 0;
  let sum = 0;
  let peak = 0;
  let clipped = 0;
  let n = 0;

  for (let f = 0; f < frames; f += stride) {
    const s = dv.getInt16(offset + f * frameBytes, true) / 32768;
    sumSq += s * s;
    sum += s;
    const a = Math.abs(s);
    if (a > peak) peak = a;
    if (a > 0.999) clipped++;
    n++;
  }

  if (!n) return { rms: 0, peak: 0, dc: 0, clipped: 0, frames, sampled: 0 };

  return {
    rms: +Math.sqrt(sumSq / n).toFixed(4),
    peak: +peak.toFixed(4),
    dc: +(sum / n).toFixed(5),
    clipped,
    frames,
    sampled: n,
  };
}

/**
 * 把 WAV 统计数字翻译成「下一步该改什么」。
 * 这一段是整个 echo 阶段的价值所在：它把硬件问题和云端问题变成可区分的事件。
 */
function diagnoseWav(wav) {
  const hints = [];

  if (wav.rms < 0.002) {
    hints.push(
      'RMS 极低（几乎静音）：优先怀疑 INMP441 的 L/R 脚悬空或接错、I2S DIN 引脚接错、' +
        '或 slot_mask 没有设成 LEFT（默认 BOTH 会读到一半空数据）'
    );
  } else if (wav.rms < 0.01) {
    hints.push(
      'RMS 偏低：INMP441 灵敏度约 -26dBFS，属正常现象。' +
        '可在固件里加数字增益（把 raw>>16 改成 raw>>14，等效 ×4）'
    );
  }

  if (wav.peak > 0.995) hints.push('峰值接近满幅：增益可能过大，容易削顶失真');
  if (wav.clipped > 0) hints.push(`检测到 ${wav.clipped} 个削顶采样点`);
  if (Math.abs(wav.dc) > 0.01) {
    hints.push(
      `直流偏置偏高 (${wav.dc})：检查 32bit 槽里取 16bit 的移位位置（应该是 raw >> 16）`
    );
  }
  if (wav.sampleRate !== 16000) {
    hints.push(`采样率 ${wav.sampleRate} 不是 16000：Whisper 会内部重采样，但建议设备端直接输出 16k`);
  }
  if (wav.channels !== 1) hints.push(`声道数 ${wav.channels} 不是 1，建议单声道以省带宽和内存`);
  if (wav.durationSec < 0.5) hints.push(`时长只有 ${wav.durationSec}s，太短，语音识别会失败`);

  if (hints.length === 0) hints.push('音频指标正常');

  return hints;
}

// ---------------------------------------------------------------------------
// AI 调用
// ---------------------------------------------------------------------------

/**
 * 语音转文字。
 *
 * ★ 关键点：Whisper 的输入 schema 是 oneOf[ string(format:binary), {audio:number[]} ]，
 *   看起来像是要传采样点数组，实际上 Cloudflare 官方教程的做法是把
 *   **整个 WAV 文件的字节做 base64 当字符串传进去** —— 模型自己识别容器格式。
 *   不需要手动解 WAV，不需要转采样深度。
 *   来源: https://developers.cloudflare.com/workers-ai/guides/tutorials/build-a-workers-ai-whisper-with-chunking/
 */
async function transcribeAudio(arrayBuffer, env) {
  const base64 = bytesToBase64(new Uint8Array(arrayBuffer));

  const res = await env.AI.run(asrModel(env), {
    audio: base64,
    task: 'transcribe',
  });

  return String(res?.text ?? '').trim();
}

/**
 * 让 LLM 把自然语言变成音乐意图（符号层）。
 *
 * ★ 重要：这个函数【绝不静默兜底】。
 *
 *   最初版本在 LLM 失败时直接返回一份写死的 fallback，结果是——
 *   语音识别正常、但音乐永远是同一首，而且响应里看不出任何异常。
 *   这种「静默失败」是最难排查的一类 bug：现象像业务问题，实际是错误被吞了。
 *
 *   现在改成把真相一并返回：intent_source 说明用的是 LLM 还是兜底，
 *   intent_error 说明兜底的原因。前端和固件端都能一眼看出问题。
 */
async function extractMusicIntent(text, env) {
  const fallback = {
    style: 'pop',
    instrument: 'nylon_guitar',
    bpm: 90,
    key: 'C',
    scale: 'major',
    bars: 4,
    arp: 'fingerpick',
  };

  try {
    const res = await env.AI.run(llmModel(env), {
      messages: [
        { role: 'system', content: INTENT_SYSTEM_PROMPT },
        { role: 'user', content: text || '用户没有说清楚，给一个通用的流行风格' },
      ],
      max_tokens: 512,
      temperature: 0.3,
    });

    const rawResp = res?.response;

    // ★★★ 根因就在这几行 ★★★
    //
    // Cloudflare Workers AI 对某些模型/输出，会把结果 JSON 直接解析成【对象】返回，
    // 而不是返回字符串。原代码写的是 extractJson(res?.response)，
    // 内部做 String(rawResp) —— 对一个对象做 String() 得到的是字面量 "[object Object]"，
    // 于是 JSON.parse 必然失败，然后静默走了兜底。
    //
    // 表现出来就是用户看到的：「识别文本是对的，但无论说什么音乐都一模一样」。
    //
    // 教训：处理第三方 API 返回值时，先判断类型再转换，永远不要假设它是字符串。
    if (rawResp && typeof rawResp === 'object') {
      return { intent: rawResp, source: 'llm', error: null, raw: JSON.stringify(rawResp) };
    }

    const rawText = String(rawResp ?? '');
    const parsed = extractJson(rawText);

    if (parsed) {
      return { intent: parsed, source: 'llm', error: null, raw: rawText };
    }

    // 模型回了话，但不是合法 JSON —— 这也是失败，必须暴露出来
    return {
      intent: fallback,
      source: 'fallback',
      error: 'llm returned non-JSON output',
      raw: rawText.slice(0, 500),
    };
  } catch (e) {
    return {
      intent: fallback,
      source: 'fallback',
      error: `llm call failed: ${String(e?.message ?? e)}`.slice(0, 400),
      raw: '',
    };
  }
}

// ---------------------------------------------------------------------------
// 请求处理
// ---------------------------------------------------------------------------

function checkAuth(request, env) {
  const expected = String(env.DEVICE_TOKEN ?? '').trim();
  if (!expected) return null; // 未配置 = 不校验

  const got = (request.headers.get('Authorization') ?? '').replace(/^Bearer\s+/i, '').trim();
  if (got === expected) return null;

  return jsonResponse(
    { ok: false, error: 'unauthorized', hint: '需要在 Authorization 头里带 Bearer <DEVICE_TOKEN>' },
    401
  );
}

async function handleIntent(request, env) {
  const t0 = Date.now();
  const stage = String(env.STAGE ?? 'echo').trim().toLowerCase();
  const requestId = (globalThis.crypto?.randomUUID?.() ?? String(Date.now())).slice(0, 8);

  // --- 读取请求体 ---
  let raw;
  try {
    raw = await request.arrayBuffer();
  } catch {
    return jsonResponse({ ok: false, stage, request_id: requestId, error: 'failed to read body' }, 400);
  }

  if (!raw.byteLength) {
    return jsonResponse({ ok: false, stage, request_id: requestId, error: 'empty request body' }, 400);
  }

  const maxSeconds = Number(env.MAX_AUDIO_SECONDS ?? 15);
  const wav = parseWav(raw);

  // 时长保护：既是防止有人上传超大文件，也是提醒你录音别太久
  if (wav.ok && wav.durationSec > maxSeconds) {
    return jsonResponse(
      {
        ok: false,
        stage,
        request_id: requestId,
        error: `audio too long: ${wav.durationSec}s > ${maxSeconds}s`,
        hint: '调大 wrangler.toml 里的 MAX_AUDIO_SECONDS，或缩短设备端录音时长',
      },
      413
    );
  }

  const audioReport = {
    bytes: raw.byteLength,
    contentType: request.headers.get('content-type') ?? '',
    ...wav,
  };
  if (wav.ok) audioReport.hints = diagnoseWav(wav);

  // ---------- 阶段 1: echo ----------
  // 只回显，不碰 AI。用来验证「ESP32 -> Cloudflare」这条链路本身是通的。
  if (stage === 'echo') {
    return jsonResponse({
      ok: wav.ok,
      stage,
      request_id: requestId,
      audio: audioReport,
      timing: { total_ms: Date.now() - t0 },
    });
  }

  // ---------- 阶段 2: mock ----------
  // 完全不调用 AI，返回固定的结构化音乐数据。
  // 硬件没到货时，ESP32 端就对着这个接口开发，等硬件到了直接切 full。
  if (stage === 'mock') {
    const expanded = expandMusicIntent({
      ...MOCK_INTENT,
      seed_text: 'mock',
      total_bars: env.TOTAL_BARS,
      drums: env.DRUMS,
    });
    return jsonResponse({
      ok: true,
      stage,
      request_id: requestId,
      text: '（mock 模式：未调用语音识别）',
      audio: audioReport,
      music: expanded.music,
      labels: expanded.labels,
      timing: { total_ms: Date.now() - t0 },
    });
  }

  // 到这里开始一定要有合法的 WAV
  if (!wav.ok) {
    return jsonResponse(
      { ok: false, stage, request_id: requestId, error: `invalid wav: ${wav.reason}`, audio: audioReport },
      400
    );
  }

  // ---------- 阶段 3: asr ----------
  if (stage === 'asr') {
    const tAsr = Date.now();
    let text = '';
    try {
      text = await transcribeAudio(raw, env);
    } catch (e) {
      return jsonResponse(
        { ok: false, stage, request_id: requestId, error: `asr failed: ${e?.message ?? e}`, audio: audioReport },
        502
      );
    }
    return jsonResponse({
      ok: true,
      stage,
      request_id: requestId,
      text,
      audio: audioReport,
      timing: { asr_ms: Date.now() - tAsr, total_ms: Date.now() - t0 },
    });
  }

  // ---------- 阶段 4: full ----------
  const tAsr = Date.now();
  let text = '';
  try {
    text = await transcribeAudio(raw, env);
  } catch (e) {
    return jsonResponse(
      { ok: false, stage, request_id: requestId, error: `asr failed: ${e?.message ?? e}`, audio: audioReport },
      502
    );
  }
  const asrMs = Date.now() - tAsr;

  const tLlm = Date.now();
  const llm = await extractMusicIntent(text, env);
  const llmMs = Date.now() - tLlm;

  // 乐理展开：LLM 出符号，这里出数字 + 编排
  // total_bars 由环境变量控制曲长；drums 可以设为 'off' 关掉鼓点
  const expanded = expandMusicIntent({
    ...llm.intent,
    seed_text: text,
    total_bars: env.TOTAL_BARS,
    drums: env.DRUMS,
  });

  return jsonResponse({
    ok: true,
    stage,
    request_id: requestId,
    text,
    audio: audioReport,
    intent: llm.intent,
    // ★ 这两个字段是排查「音乐永远一样」的关键：
    //   intent_source = 'llm'       -> 真的用了模型，输出会跟着你说的话变
    //   intent_source = 'fallback'  -> 模型失败了，用的是写死的默认编排（所以永远一样）
    intent_source: llm.source,
    intent_error: llm.error,
    intent_raw: llm.raw,
    music: expanded.music,
    labels: expanded.labels,
    timing: {
      asr_ms: asrMs,
      llm_ms: llmMs,
      total_ms: Date.now() - t0,
    },
  });
}

/**
 * 生成一段极短的测试 WAV（1 秒 / 16kHz / 单声道 / 16bit）。
 * 给 /api/v1/aitest?asr=1 用 —— 只要求「能被解码」，不要求内容有意义。
 * 加一点双音正弦，避免纯静音被某些实现直接拒绝。
 */
function selfTestWav(seconds = 1, sampleRate = 16000) {
  const n = Math.round(seconds * sampleRate);
  const buf = new ArrayBuffer(44 + n * 2);
  const dv = new DataView(buf);
  const w = (o, s) => {
    for (let i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i));
  };
  w(0, 'RIFF');
  dv.setUint32(4, 36 + n * 2, true);
  w(8, 'WAVE');
  w(12, 'fmt ');
  dv.setUint32(16, 16, true);
  dv.setUint16(20, 1, true);
  dv.setUint16(22, 1, true);
  dv.setUint32(24, sampleRate, true);
  dv.setUint32(28, sampleRate * 2, true);
  dv.setUint16(32, 2, true);
  dv.setUint16(34, 16, true);
  w(36, 'data');
  dv.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) {
    const t = i / sampleRate;
    const s = 0.08 * Math.sin(2 * Math.PI * 220 * t) + 0.05 * Math.sin(2 * Math.PI * 440 * t);
    dv.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, s)) * 32767), true);
  }
  return buf;
}

// ---------------------------------------------------------------------------
// 定时自检
// ---------------------------------------------------------------------------

/**
 * Cron 触发的 AI 自检。
 *
 * 存在的唯一目的：**远程验证「部署在 Cloudflare 上的 Worker 能不能调 AI」**。
 *
 * 为什么需要它：中国大陆的 `*.workers.dev` 被墙，本地 `wrangler dev` 调 AI 又超时，
 * 于是没有任何办法从本地测出「部署后 AI 到底通不通」。
 * 但 Cron 是由 Cloudflare 自己触发的，日志可以用 `wrangler tail` 读回来
 * （tail 走 api.cloudflare.com，这个地址在国内是通的）。
 *
 * 日志里会打出一行 `AI_SELFTEST {...}`，用 tail 搜这个标记即可。
 */
async function aiSelfTest(env) {
  const t0 = Date.now();
  let record;

  try {
    const r = await env.AI.run(llmModel(env), {
      messages: [{ role: 'user', content: 'Reply with exactly one word: OK' }],
      max_tokens: 8,
    });
    record = {
      ok: true,
      ms: Date.now() - t0,
      ts: new Date().toISOString(),
      model: llmModel(env),
      reply: String(r?.response ?? '').slice(0, 80),
    };
  } catch (e) {
    record = {
      ok: false,
      ms: Date.now() - t0,
      ts: new Date().toISOString(),
      model: llmModel(env),
      error: String(e?.message ?? e).slice(0, 300),
    };
  }

  console.log(`AI_SELFTEST ${JSON.stringify(record)}`);

  // 写进 KV。
  // 为什么需要：中国大陆 `wrangler tail` 走的 tail.developers.workers.dev 被墙，
  // 日志根本读不回来。但 `wrangler kv key get` 走的是 api.cloudflare.com（可达），
  // 所以把结果落到 KV 是唯一能远程观测的途径。
  if (env.SELFTEST_KV) {
    try {
      await env.SELFTEST_KV.put('last', JSON.stringify(record, null, 2));
    } catch (e) {
      console.log(`AI_SELFTEST_KV_ERROR ${String(e?.message ?? e)}`);
    }
  }
}

// ---------------------------------------------------------------------------
// 路由
// ---------------------------------------------------------------------------

export default {
  async scheduled(event, env, ctx) {
    // 先立刻写一个「cron 已触发」的标记，然后才去调 AI。
    // 这样能把两种情况分开：
    //   连标记都没有     -> cron 压根没触发（触发器配置或账号问题）
    //   有标记但没结果   -> cron 跑了，但 AI 调用挂住没返回
    if (env.SELFTEST_KV) {
      try {
        await env.SELFTEST_KV.put(
          'cron_fired',
          JSON.stringify(
            {
              ts: new Date().toISOString(),
              cron: event?.cron ?? '(unknown)',
              scheduledTime: event?.scheduledTime ?? null,
            },
            null,
            2
          )
        );
      } catch (e) {
        console.log(`AI_SELFTEST_MARKER_ERROR ${String(e?.message ?? e)}`);
      }
    }

    await aiSelfTest(env);
  },

  async fetch(request, env) {
    const url = new URL(request.url);

    // CORS 预检
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    // 浏览器录音测试页 —— 硬件没到货时用它做端到端验证
    if (request.method === 'GET' && (url.pathname === '/' || url.pathname === '/demo')) {
      return new Response(DEMO_PAGE, {
        headers: {
          'Content-Type': 'text/html; charset=utf-8',
          // ★ 绝不能缓存：这个页面就是我迭代调试用的，缓存住旧版会让人
          //   误以为"部署没生效"。实测被缓存坑过一次。
          'Cache-Control': 'no-store, must-revalidate',
        },
      });
    }

    // 健康检查 / 探活
    if (url.pathname === '/api/v1/health') {
      return jsonResponse({
        ok: true,
        service: 'music-intent-api',
        stage: String(env.STAGE ?? 'echo'),
        auth_required: Boolean(String(env.DEVICE_TOKEN ?? '').trim()),
        ai_binding: Boolean(env.AI),
        asr_model: asrModel(env),
        llm_model: llmModel(env),
        ts: new Date().toISOString(),
      });
    }

    // ---------------------------------------------------------------------
    // AI 自检：只调一次最便宜的 LLM
    //
    // 为什么需要它：ASR 失败时，你无法直接判断是「AI 网络不通」还是
    // 「Whisper 对这坨音频不满意」。这个接口把两件事分开——
    // 它只调 LLM（输入输出都极小），如果它通了，说明 AI 链路是好的。
    // ---------------------------------------------------------------------
    if (url.pathname === '/api/v1/aitest') {
      const modelParam = url.searchParams.get('model');

      // ---- 模式 C: ?intent=<文本>
      // 用【真实的系统提示词】跑一次意图解析，并把模型的原始输出原样吐出来。
      // 这是排查「LLM 返回非 JSON」最快的手段——不用录音、不用猜。
      const intentText = url.searchParams.get('intent');
      if (intentText !== null) {
        const t0 = Date.now();
        const target = modelParam || llmModel(env);
        try {
          const r = await env.AI.run(target, {
            messages: [
              { role: 'system', content: INTENT_SYSTEM_PROMPT },
              { role: 'user', content: intentText || '随便来一首' },
            ],
            max_tokens: 512,
            temperature: 0.3,
          });
          const rawResp = r?.response;
          const respType =
            rawResp === null ? 'null' : Array.isArray(rawResp) ? 'array' : typeof rawResp;
          // 对象就直接用；字符串才去解析
          const parsed =
            rawResp && typeof rawResp === 'object'
              ? rawResp
              : extractJson(String(rawResp ?? ''));
          const raw =
            typeof rawResp === 'string' ? rawResp : JSON.stringify(rawResp);

          return jsonResponse({
            kind: 'intent',
            model: target,
            ms: Date.now() - t0,
            input: intentText,
            response_type: respType, // ★ 一眼看出模型返回的是 string 还是 object
            parsed_ok: Boolean(parsed),
            parsed,
            raw,
            raw_length: raw ? raw.length : 0,
          });
        } catch (e) {
          return jsonResponse(
            { kind: 'intent', model: target, ms: Date.now() - t0, error: String(e?.message ?? e).slice(0, 300) },
            502
          );
        }
      }

      // ---- 模式 A: ?asr=1  测语音识别 ----
      if (url.searchParams.get('asr')) {
        const t0 = Date.now();
        const target = modelParam || asrModel(env);
        try {
          const r = await env.AI.run(target, {
            audio: bytesToBase64(new Uint8Array(selfTestWav(1))),
            task: 'transcribe',
          });
          return jsonResponse({
            ok: true,
            kind: 'asr',
            model: target,
            ms: Date.now() - t0,
            text: String(r?.text ?? ''),
          });
        } catch (e) {
          return jsonResponse(
            { ok: false, kind: 'asr', model: target, ms: Date.now() - t0, error: String(e?.message ?? e).slice(0, 300) },
            502
          );
        }
      }

      // ---- 模式 B: 默认测 LLM，可用 ?model= 换模型 ----
      const target = modelParam || llmModel(env);
      const t0 = Date.now();
      try {
        const r = await env.AI.run(target, {
          messages: [{ role: 'user', content: 'Reply with exactly one word: OK' }],
          max_tokens: 8,
        });
        return jsonResponse({
          ok: true,
          kind: 'llm',
          model: target,
          ms: Date.now() - t0,
          reply: String(r?.response ?? '').slice(0, 80),
        });
      } catch (e) {
        return jsonResponse(
          { ok: false, kind: 'llm', model: target, ms: Date.now() - t0, error: String(e?.message ?? e).slice(0, 300) },
          502
        );
      }
    }

    // 主接口
    if (url.pathname === '/api/v1/intent') {
      if (request.method !== 'POST') {
        return jsonResponse({ ok: false, error: 'use POST' }, 405);
      }
      const denied = checkAuth(request, env);
      if (denied) return denied;

      const res = await handleIntent(request, env);

      // ?pretty=0 -> 紧凑输出（去掉缩进），响应体积大约减半。
      //
      // ★ 固件端建议用这个：16 小节的曲子在美化输出下约 6.2KB，
      //   紧凑输出只要 2.6KB，接收缓冲可以从 10KB 降到 5KB。
      //   浏览器和 curl 用默认的美化输出，方便人看。
      if (url.searchParams.get('pretty') === '0') {
        try {
          const body = await res.json();
          return new Response(JSON.stringify(body), {
            status: res.status,
            headers: res.headers,
          });
        } catch {
          return res; // 不是 JSON（理论上不会发生），原样返回
        }
      }
      return res;
    }

    return jsonResponse({ ok: false, error: 'not found', path: url.pathname }, 404);
  },
};
