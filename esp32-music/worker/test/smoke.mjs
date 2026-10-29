/**
 * 冒烟测试：打真实部署好的 Worker
 * ===========================================================================
 *
 * 和 local.mjs 的区别：
 *   local.mjs —— 直接把 fetch 函数 import 进来调用，不联网，测「逻辑」
 *   smoke.mjs —— 用真的 HTTP 请求打真实地址，测「部署是否真的能用」
 *
 * 这两个测试都不能省：
 *   local.mjs 通过但 smoke.mjs 失败 -> 问题在部署/配置/账号
 *   两个都通过                      -> 云端整条链是通的
 *
 * 用法：
 *   node test/smoke.mjs
 *   node test/smoke.mjs https://music-intent-api.xxx.workers.dev
 *   set WORKER_URL=https://... && node test/smoke.mjs
 */

const BASE =
  process.argv[2] ||
  process.env.WORKER_URL ||
  'https://music-intent-api.2356766287.workers.dev';

const TOKEN = process.env.DEVICE_TOKEN || '';

let passed = 0;
let failed = 0;

function check(cond, label, detail) {
  if (cond) {
    passed++;
    console.log(`  ✓ ${label}`);
  } else {
    failed++;
    console.log(`  ✗ ${label}${detail ? `\n      ${detail}` : ''}`);
  }
}

function section(name) {
  console.log(`\n${name}`);
}

/** 生成一段 N 秒的 16kHz 单声道 16bit WAV */
function makeWav(seconds = 3, sampleRate = 16000, amplitude = 0.25) {
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
    const s =
      0.10 * Math.sin(2 * Math.PI * 180 * t) +
      0.08 * Math.sin(2 * Math.PI * 240 * t) +
      0.05 * Math.sin(2 * Math.PI * 320 * t);
    dv.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, s)) * 32767), true);
  }
  return buf;
}

function headers(extra = {}) {
  const h = { ...extra };
  if (TOKEN) h.Authorization = `Bearer ${TOKEN}`;
  return h;
}

console.log(`目标地址: ${BASE}`);
console.log('='.repeat(64));

// ---------------------------------------------------------------------------
// 1. 探活
// ---------------------------------------------------------------------------
section('1. GET /api/v1/health');

let health = null;
try {
  const t0 = Date.now();
  const res = await fetch(`${BASE}/api/v1/health`);
  const ms = Date.now() - t0;
  health = await res.json();

  check(res.status === 200, `HTTP 200（耗时 ${ms}ms）`, `实际 ${res.status}`);
  check(health.service === 'music-intent-api', '服务名正确', JSON.stringify(health));
  check(typeof health.stage === 'string', `当前 STAGE = ${health.stage}`);
  check(health.ai_binding === true, 'AI 绑定已生效', `实际 ai_binding=${health.ai_binding}`);
  console.log(`      auth_required = ${health.auth_required}`);
} catch (e) {
  check(false, '探活请求成功', String(e?.message ?? e));
  console.log('\n无法连接到 Worker，后面的测试跳过。');
  process.exit(1);
}

// ---------------------------------------------------------------------------
// 2. 测试页
// ---------------------------------------------------------------------------
section('2. GET / （浏览器测试台）');

try {
  const res = await fetch(BASE);
  const html = await res.text();
  check(res.status === 200, 'HTTP 200', `实际 ${res.status}`);
  check(
    (res.headers.get('content-type') || '').includes('text/html'),
    'Content-Type 是 HTML',
    res.headers.get('content-type')
  );
  check(html.includes('Karplus'), '页面含 Karplus-Strong 合成器');
  check(html.includes('encodeWav'), '页面含 WAV 编码器');
  console.log(`      页面大小 ${Buffer.byteLength(html)} 字节`);
} catch (e) {
  check(false, '测试页请求成功', String(e?.message ?? e));
}

// ---------------------------------------------------------------------------
// 3. 主接口：Post 真实 WAV
// ---------------------------------------------------------------------------
section(`3. POST /api/v1/intent（STAGE=${health.stage}）`);

const wav = makeWav(3);
console.log(`      上传 3 秒 WAV，共 ${wav.byteLength} 字节`);

let body = null;
try {
  const t0 = Date.now();
  const res = await fetch(`${BASE}/api/v1/intent`, {
    method: 'POST',
    headers: headers({ 'Content-Type': 'audio/wav' }),
    body: wav,
  });
  const ms = Date.now() - t0;
  body = await res.json();

  check(res.status === 200, `HTTP 200（往返 ${ms}ms）`, `实际 ${res.status}：${JSON.stringify(body)}`);
  check(body.ok === true, 'ok = true', JSON.stringify(body.error));
  check(typeof body.request_id === 'string', `request_id = ${body.request_id}`);

  // 音频诊断
  check(body.audio?.ok === true, '后端解析 WAV 成功', body.audio?.reason);
  check(body.audio?.sampleRate === 16000, `采样率识别 = ${body.audio?.sampleRate}`);
  check(body.audio?.channels === 1, `声道数识别 = ${body.audio?.channels}`);
  check(body.audio?.durationSec === 3, `时长识别 = ${body.audio?.durationSec}s`);
  console.log(`      RMS = ${body.audio?.rms}  峰值 = ${body.audio?.peak}`);
  if (body.audio?.hints) {
    body.audio.hints.forEach((h) => console.log(`      诊断: ${h}`));
  }

  // ★ 意图来源：这是「音乐永远一样」那个 bug 的直接观测点。
  //   full 阶段必须是 llm；如果是 fallback，说明 LLM 那一步出了问题。
  if (health.stage === 'full') {
    check(
      body.intent_source === 'llm',
      `意图来源 = llm（不是 fallback）`,
      `实际 intent_source=${body.intent_source}，intent_error=${body.intent_error}`
    );
    if (body.intent_raw) {
      console.log(`      模型原始输出: ${String(body.intent_raw).slice(0, 160)}`);
    }
  }

  // 音乐数据
  section('4. 返回的音乐数据');
  if (body.music) {
    const m = body.music;
    check(typeof m.bpm === 'number', `bpm = ${m.bpm}`);
    check(typeof m.step_ms === 'number', `step_ms = ${m.step_ms}`);
    check(Array.isArray(m.chord_notes) && m.chord_notes.length > 0, `chord_notes 有 ${m.chord_notes?.length} 个音`);
    check(Array.isArray(m.melody_notes) && m.melody_notes.length > 0, `melody_notes 有 ${m.melody_notes?.length} 格`);
    check(
      m.melody_notes?.every((n) => n === -1 || (n >= 0 && n <= 127)),
      '旋律音高全在 MIDI 合法范围'
    );
    check(
      m.chord_notes?.every((n) => n >= 0 && n <= 127),
      '和弦音高全在 MIDI 合法范围'
    );
    check(typeof m.decay_seconds === 'number', `decay_seconds = ${m.decay_seconds}`);
    check(typeof m.brightness === 'number', `brightness = ${m.brightness}`);

    // 扩展 KS 参数
    check(typeof m.damping === 'number', `环路阻尼 damping = ${m.damping}`);
    check(typeof m.pick_position === 'number', `拨弦位置 pick_position = ${m.pick_position}`);
    check(m.damping >= 0 && m.damping <= 0.6, 'damping 在合理范围 0..0.6');
    check(m.pick_position >= 0 && m.pick_position <= 0.5, 'pick_position 在合理范围 0..0.5');

    // 鼓点
    check(Array.isArray(m.drum_kick), `drum_kick 长度 ${m.drum_kick?.length}`);
    check(m.drum_kick?.length === 16, '鼓点网格固定为每小节 16 格（16 分音符）');
    check(m.drum_snare?.length === 16, 'drum_snare 也是 16 格');
    check(m.drum_hat?.length === 16, 'drum_hat 也是 16 格');
    check(typeof m.drum_step_ms === 'number', `鼓点每格 = ${m.drum_step_ms}ms`);
    console.log(`      鼓型  : ${m.drum_pattern}`);
    console.log(`      kick  : ${m.drum_kick?.join('')}`);
    console.log(`      snare : ${m.drum_snare?.join('')}`);
    console.log(`      hat   : ${m.drum_hat?.join('')}`);

    // 乐句结构：同一段落内应当出现动机重复，而不是纯随机游走。
    // ★ 注意要在【同一个段落内】比较：前奏/尾奏本来就不弹旋律，
    //   拿前奏小节去和有旋律的小节比节奏是没有意义的（踩过这个坑）。
    if (m.sections && m.sections.length) {
      let cursor = 0;
      let target = null;
      for (const s of m.sections) {
        if (s.name === 'verse' || s.name === 'main') {
          target = { sec: s, start: cursor };
          break;
        }
        cursor += s.bars;
      }
      if (!target) target = { sec: m.sections[0], start: 0 };

      if (target.sec.bars >= 4) {
        const spb2 = m.beats_per_bar * m.steps_per_beat;
        const rhythm = (b) =>
          m.melody_notes.slice(b * spb2, (b + 1) * spb2).map((n) => (n >= 0 ? 1 : 0)).join('');
        const r0 = rhythm(target.start);
        const r2 = rhythm(target.start + 2);
        check(
          r0 === r2,
          `乐句结构：${target.sec.name} 段第 1、3 小节节奏一致（动机再现）`,
          `r0=${r0} r2=${r2}`
        );
      }
      console.log(`      曲式: ${body.labels?.form}`);
    }

    console.log(`      调式: ${body.labels?.key} ${body.labels?.scale}`);
    console.log(`      和弦: ${body.labels?.chords?.join(' - ')}`);
    console.log(`      音色: ${body.labels?.instrument}`);
    console.log(`      和弦音高: [${m.chord_notes.join(', ')}]`);
    console.log(`      旋律音高: [${m.melody_notes.join(', ')}]`);
  } else {
    check(false, 'music 字段存在', '响应里没有 music');
  }
} catch (e) {
  check(false, '主接口请求成功', String(e?.message ?? e));
}

// ---------------------------------------------------------------------------
// 5. 错误处理
// ---------------------------------------------------------------------------
section('5. 错误处理');

try {
  const res = await fetch(`${BASE}/api/v1/intent`, {
    method: 'POST',
    headers: headers({ 'Content-Type': 'audio/wav' }),
    body: new Uint8Array(200).fill(0x41), // 200 个 'A'，不是 WAV
  });
  const j = await res.json();
  // echo / mock 阶段不校验 WAV 合法性，只有 asr / full 才会 400
  const expectHardFail = health.stage === 'asr' || health.stage === 'full';
  if (expectHardFail) {
    check(res.status === 400, '非法 WAV 返回 400', `实际 ${res.status}`);
  } else {
    check(res.status === 200, `${health.stage} 阶段不拦截非法 WAV（符合设计）`);
  }
  check(j.audio?.ok === false, 'audio.ok = false');
  check(typeof j.audio?.reason === 'string', `给出原因: ${j.audio?.reason}`);
} catch (e) {
  check(false, '非法 WAV 请求成功', String(e?.message ?? e));
}

try {
  const res = await fetch(`${BASE}/nonexistent`);
  check(res.status === 404, '未知路径返回 404', `实际 ${res.status}`);
} catch (e) {
  check(false, '404 测试成功', String(e?.message ?? e));
}

// ---------------------------------------------------------------------------
// 汇总
// ---------------------------------------------------------------------------
console.log('\n' + '='.repeat(64));
if (failed === 0) {
  console.log(`✅ 冒烟测试全部通过：${passed} 项`);
  console.log(`\n浏览器打开看看: ${BASE}`);
  // 用 exitCode 而不是 process.exit()：Windows 上 fetch 还有未关闭的句柄时
  // 强行 process.exit() 会触发 libuv 的 UV_HANDLE_CLOSING 断言崩溃
  process.exitCode = 0;
} else {
  console.log(`❌ 失败 ${failed} 项 / 共 ${passed + failed} 项`);
  process.exitCode = 1;
}
