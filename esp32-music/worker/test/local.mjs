/**
 * 离线测试
 * ===========================================================================
 *
 * 不需要 wrangler、不需要 Cloudflare 账号、不需要联网。
 * 直接 import Worker 模块，用一个假的 env 调用它的 fetch 处理器。
 *
 * 这么做的意义：Worker 用的是 Web 标准 API（Request / Response / DataView /
 * btoa / crypto），Node 18+ 全部原生支持。所以「本地跑不通、部署了才发现」
 * 这类问题可以完全避免 —— 而这正是三天时间线里最不该浪费的时间。
 *
 * 跑法：npm test
 */

import { parseChord, expandMusicIntent, CHORD_TYPES, SCALES, INSTRUMENTS, ARP_PATTERNS } from '../src/music.js';
import { DEMO_PAGE } from '../src/demo-page.js';
import worker from '../src/index.js';

// ---------------------------------------------------------------------------
// 极简断言
// ---------------------------------------------------------------------------

let passed = 0;
let failed = 0;
const failures = [];

function check(cond, label, detail) {
  if (cond) {
    passed++;
  } else {
    failed++;
    failures.push({ label, detail });
    console.log(`  ✗ ${label}${detail ? `  ->  ${detail}` : ''}`);
  }
}

function eq(actual, expected, label) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  check(a === e, label, `期望 ${e}，实际 ${a}`);
}

function section(name) {
  console.log(`\n${name}`);
}

// ---------------------------------------------------------------------------
// 测试用 WAV 生成器
// ---------------------------------------------------------------------------

function makeWav({ seconds = 1, sampleRate = 16000, freq = 220, amplitude = 0.3 } = {}) {
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
    const s = Math.sin((2 * Math.PI * freq * i) / sampleRate) * amplitude;
    dv.setInt16(44 + i * 2, Math.round(s * 32767), true);
  }
  return buf;
}

/** 假的 env：AI 绑定被替换成可预测的桩，并记录被调用了哪个模型 */
function makeEnv(overrides = {}) {
  const aiCalls = [];
  const env = {
    STAGE: 'echo',
    DEVICE_TOKEN: '',
    MAX_AUDIO_SECONDS: '15',
    AI: {
      async run(model, input) {
        aiCalls.push({ model, input });
        if (String(model).includes('whisper')) {
          return { text: '给我来一首欢快的音乐' };
        }
        return {
          response:
            '```json\n' +
            JSON.stringify({
              style: 'mandopop',
              instrument: 'nylon_guitar',
              bpm: 88,
              key: 'G',
              scale: 'major',
              bars: 4,
              chords: ['Gmaj7', 'Em7', 'Am7', 'D7'],
              arp: 'fingerpick',
            }) +
            '\n```',
        };
      },
    },
    ...overrides,
  };
  env.aiCalls = aiCalls;
  return env;
}

async function post(body, env, headers = {}, path = '/api/v1/intent') {
  const req = new Request(`https://test.local${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'audio/wav', ...headers },
    body,
  });
  const res = await worker.fetch(req, env);
  const text = await res.text();
  let json = null;
  try {
    json = JSON.parse(text);
  } catch {
    /* 非 JSON 响应（比如 HTML 页面） */
  }
  return { status: res.status, json, text };
}

async function get(path, env) {
  const res = await worker.fetch(new Request(`https://test.local${path}`), env);
  const text = await res.text();
  let json = null;
  try {
    json = JSON.parse(text);
  } catch {
    /* ignore */
  }
  return { status: res.status, json, text, contentType: res.headers.get('content-type') };
}

// ===========================================================================
// 1. 乐理：和弦解析
// ===========================================================================

section('1. 和弦符号解析');

eq(parseChord('C'), { root: 0, typeIndex: 0, intervals: [0, 4, 7], symbol: 'C' }, "C -> 大三和弦");
eq(parseChord('Am').typeIndex, 1, 'Am -> 小三和弦 (index 1)');
eq(parseChord('Am').root, 9, 'Am 根音是 A = 9');
eq(parseChord('F#m7').root, 6, 'F#m7 根音是 F# = 6');
eq(parseChord('F#m7').typeIndex, 3, 'F#m7 -> 小七 (index 3)');
eq(parseChord('Bbmaj7').root, 10, 'Bbmaj7 根音是 A# = 10');
eq(parseChord('Bbmaj7').typeIndex, 2, 'Bbmaj7 -> 大七 (index 2)');
eq(parseChord('G7').typeIndex, 4, 'G7 -> 属七 (index 4)');
eq(parseChord('Dm7b5').typeIndex, 11, 'Dm7b5 -> 半减七 (index 11)，不能被 m7 抢先匹配');
eq(parseChord('C/E').typeIndex, 0, 'C/E 转位按 C 处理');
eq(parseChord('C/E').root, 0, 'C/E 根音仍是 C');
eq(parseChord('Csus4').typeIndex, 7, 'Csus4 -> 挂四');
eq(parseChord('Cadd9').typeIndex, 12, 'Cadd9 -> 加九');
eq(parseChord('Cmaj7').typeIndex, 2, 'Cmaj7 -> 大七');
eq(parseChord('Cmin7').typeIndex, 3, 'Cmin7 -> 小七（别名归一化）');
eq(parseChord('CM7').typeIndex, 2, 'CM7 -> 大七（爵士写法）');
eq(parseChord('garbage'), null, '无法解析的和弦返回 null（不能被静默当成 G 大三和弦）');
eq(parseChord('N.C.'), null, 'N.C.（no chord）被拒绝');
eq(parseChord(''), null, '空字符串不是合法和弦');
eq(parseChord('Cmaj9').typeIndex, 2, '"Cmaj9" -> maj7（延伸音隐含七度音，不丢成普通 C）');
eq(parseChord('Cm9').typeIndex, 3, '"Cm9" -> min7');
eq(parseChord('C9').typeIndex, 4, '"C9" -> 属七（C E G Bb D），不是大三和弦');
eq(parseChord('C7sus4').typeIndex, 4, '"7sus4" 至少保住属七性质，不返回 null');

// ===========================================================================
// 2. 乐理：意图展开
// ===========================================================================

section('2. 音乐意图展开');

const ex = expandMusicIntent({
  key: 'C',
  scale: 'major',
  bpm: 120,
  total_bars: 4, // ★ 曲长现在是独立参数；intent.bars 不再决定总长度
  chords: ['Cmaj7', 'Am7', 'Dm7', 'G7'],
  arp: 'fingerpick',
  instrument: 'nylon_guitar',
  seed_text: 'test',
});

eq(ex.music.bpm, 120, 'bpm 透传');
eq(ex.music.key_index, 0, 'C -> key_index 0');
eq(ex.music.scale_index, 0, 'major -> scale_index 0');
eq(ex.music.bars, 4, 'bars 透传');
eq(ex.music.beats_per_bar, 4, '默认 4 拍/小节');
eq(ex.music.steps_per_beat, 2, '默认每拍 2 格（八分音符）');
eq(ex.music.step_ms, Math.round(60000 / 120 / 2), 'step_ms = 60000/bpm/steps_per_beat = 250');
eq(ex.music.step_ms, 250, 'step_ms 具体值校验');
eq(ex.music.chord_note_counts, [4, 4, 4, 4], '四个七和弦各 4 个音');
eq(ex.music.chord_notes.length, 16, '和弦音总数 = 4 小节 × 4 音');
eq(ex.labels.chords, ['Cmaj7', 'Am7', 'Dm7', 'G7'], 'labels.chords 保留原始符号');
eq(ex.labels.key, 'C', 'labels.key 可读');
eq(ex.labels.instrument, 'nylon_guitar', 'labels.instrument 可读');
eq(ex.music.arp_pattern, ARP_PATTERNS.fingerpick, 'fingerpick 音型正确');

check(
  ex.music.chord_notes.every((n) => n >= 0 && n <= 127),
  '所有和弦音高在 MIDI 合法范围 0..127',
  JSON.stringify(ex.music.chord_notes)
);
check(
  ex.music.melody_notes.every((n) => n === -1 || (n >= 60 && n <= 84)),
  '旋律音高都在 C4..C6 音域内（-1 为休止）',
  JSON.stringify(ex.music.melody_notes)
);
check(
  ex.music.melody_notes.length === 4 * 4 * 2,
  `旋律长度 = bars × 拍/小节 × 格/拍 = 32，实际 ${ex.music.melody_notes.length}`
);
check(
  ex.music.melody_notes.some((n) => n >= 0),
  '旋律里至少有一个发声的音'
);
check(
  ex.music.chord_notes.every((n) => n >= 48 && n <= 72),
  '和弦声位落在 C3..C5 伴奏音域',
  JSON.stringify(ex.music.chord_notes)
);

// 确定性：同样输入必须同样输出，否则没法复现 bug
const ex2 = expandMusicIntent({
  key: 'C',
  scale: 'major',
  bpm: 120,
  total_bars: 4,
  chords: ['Cmaj7', 'Am7', 'Dm7', 'G7'],
  arp: 'fingerpick',
  instrument: 'nylon_guitar',
  seed_text: 'test',
});
eq(ex2.music.melody_notes, ex.music.melody_notes, '确定性：同样输入 -> 同样旋律（可复现）');

const ex3 = expandMusicIntent({ seed_text: '完全不同的输入 abcxyz', bpm: 90 });
check(
  JSON.stringify(ex3.music.melody_notes) !== JSON.stringify(ex.music.melody_notes),
  '不同 seed_text 产生不同旋律'
);

// 兜底
const fallback = expandMusicIntent({});
eq(fallback.labels.chords.length, 4, '不给和弦时兜底为 I-vi-ii-V 四小节素材');
eq(fallback.music.bars, 16, '不指定曲长时默认 16 小节');
check(fallback.music.bpm === 90, '兜底 bpm = 90');
const fallbackMinor = expandMusicIntent({ scale: 'minor', key: 'A' });
eq(fallbackMinor.music.key_index, 9, 'A 小调 key_index = 9');
eq(fallbackMinor.music.scale_index, 1, 'minor scale_index = 1');
eq(fallbackMinor.labels.chords, ['Am7', 'Fmaj7', 'Cmaj7', 'G7'], '小调兜底走向 i-VI-III-VII');
// 自然小调的 III 级必须是大三和弦。写成 Cmin7 就不是调内和弦，一听就错。
eq(parseChord(fallbackMinor.labels.chords[2]).typeIndex, 2, '小调 III 级是大三和弦，不是小三和弦');
eq(parseChord(fallbackMinor.labels.chords[1]).typeIndex, 2, '小调 VI 级是大三和弦');

// 越界保护
const clamped = expandMusicIntent({ bpm: 99999, total_bars: 99, steps_per_beat: 99 });
check(clamped.music.bpm === 220, `bpm 上限钳制到 220，实际 ${clamped.music.bpm}`);
check(clamped.music.bars === 32, `曲长上限钳制到 32，实际 ${clamped.music.bars}`);
check(clamped.music.steps_per_beat === 4, `steps_per_beat 上限钳制到 4`);
check(
  clamped.music.melody_notes.length ===
    clamped.music.bars * clamped.music.beats_per_bar * clamped.music.steps_per_beat,
  `旋律长度严格等于 曲长 × 拍/小节 × 格/拍（实际 ${clamped.music.melody_notes.length}）`,
  JSON.stringify({ bars: clamped.music.bars, bpb: clamped.music.beats_per_bar, spb: clamped.music.steps_per_beat })
);
check(
  clamped.music.melody_notes.length <= 1024,
  `旋律长度有硬上限 1024（32 小节 × 8 拍 × 4 格），实际 ${clamped.music.melody_notes.length}`
);

// 音色
eq(INSTRUMENTS[0].key, 'nylon_guitar', '音色表 0 号是尼龙弦吉他');
check(
  INSTRUMENTS.every((i) => i.decay_seconds > 0 && i.brightness >= 0 && i.brightness <= 1),
  '所有音色的 decay_seconds / brightness 都在合理范围'
);
const harp = expandMusicIntent({ instrument: 'harp', seed_text: 'x' });
eq(harp.music.instrument_index, 3, 'harp -> instrument_index 3');
eq(harp.music.decay_seconds, INSTRUMENTS[3].decay_seconds, 'decay_seconds 透传');

check(
  SCALES.every((s) => s.steps.length >= 5 && s.steps[0] === 0),
  '所有音阶都以主音 0 起始且至少 5 个音'
);
check(
  CHORD_TYPES.every((c) => c.intervals[0] === 0),
  '所有和弦类型的第一个音程都是 0（根音）'
);

// ===========================================================================
// 3. WAV 解析（echo 阶段）
// ===========================================================================

section('3. WAV 解析与音频诊断');

{
  const env = makeEnv({ STAGE: 'echo' });
  const r = await post(makeWav({ seconds: 2, amplitude: 0.3 }), env);

  eq(r.status, 200, 'echo 阶段返回 200');
  eq(r.json.stage, 'echo', 'stage 回显正确');
  eq(r.json.audio.ok, true, '合法 WAV 解析成功');
  eq(r.json.audio.sampleRate, 16000, '采样率识别正确');
  eq(r.json.audio.channels, 1, '声道数识别正确');
  eq(r.json.audio.bitsPerSample, 16, '位深识别正确');
  eq(r.json.audio.durationSec, 2, '时长计算正确 = 2 秒');
  eq(r.json.audio.bytes, 44 + 2 * 16000 * 2, '字节数正确');

  // 振幅 0.3 的正弦波，RMS 理论值 = 0.3/√2 ≈ 0.2121
  check(
    Math.abs(r.json.audio.rms - 0.2121) < 0.01,
    `RMS 计算正确（期望 ≈0.2121，实际 ${r.json.audio.rms}）`
  );
  check(r.json.audio.peak > 0.29 && r.json.audio.peak <= 0.31, `峰值正确（实际 ${r.json.audio.peak}）`);
  check(Array.isArray(r.json.audio.hints) && r.json.audio.hints.length > 0, '给出了诊断建议');
  check(env.aiCalls.length === 0, 'echo 阶段绝不调用 AI（这是它的全部意义）');
}

{
  // 静音音频 -> 应该给出「RMS 极低」的诊断
  const env = makeEnv({ STAGE: 'echo' });
  const r = await post(makeWav({ seconds: 1, amplitude: 0 }), env);
  check(r.json.audio.ok, '静音 WAV 仍是合法 WAV');
  check(r.json.audio.rms === 0, '静音 RMS = 0');
  check(
    r.json.audio.hints.some((h) => h.includes('极低')),
    '静音时给出「RMS 极低」的硬件排查建议',
    JSON.stringify(r.json.audio.hints)
  );
}

{
  // 采样率不是 16000 -> 应该被提示
  const env = makeEnv({ STAGE: 'echo' });
  const r = await post(makeWav({ seconds: 1, sampleRate: 44100 }), env);
  eq(r.json.audio.sampleRate, 44100, '44.1kHz 被正确识别');
  check(
    r.json.audio.hints.some((h) => h.includes('16000')),
    '非 16kHz 时给出重采样提示'
  );
}

{
  // 各种畸形输入
  const env = makeEnv({ STAGE: 'echo' });

  const garbage = new Uint8Array(200).fill(0x41).buffer; // 200 个 'A'
  const r1 = await post(garbage, env);
  eq(r1.json.audio.ok, false, '非 RIFF 数据被拒绝');
  check(r1.json.audio.reason.includes('RIFF'), '错误信息指出缺 RIFF magic', r1.json.audio.reason);

  const tiny = new Uint8Array(10).buffer;
  const r2 = await post(tiny, env);
  eq(r2.json.audio.ok, false, '过短数据被拒绝');
  check(r2.json.audio.reason.includes('too short'), '错误信息指出长度不足');

  const empty = new ArrayBuffer(0);
  const r3 = await post(empty, env);
  eq(r3.status, 400, '空 body 返回 400');
  eq(r3.json.error, 'empty request body', '空 body 错误信息正确');
}

{
  // 32bit 浮点 WAV（audioFormat=3）应该被明确拒绝，而不是算出垃圾数据
  const wav32 = makeWav({ seconds: 0.5 });
  const dv = new DataView(wav32);
  dv.setUint16(20, 3, true); // audioFormat = IEEE float
  dv.setUint16(34, 32, true); // 32 bit
  const env = makeEnv({ STAGE: 'echo' });
  const r = await post(wav32, env);
  eq(r.json.audio.ok, false, '非 PCM 格式被拒绝');
  check(r.json.audio.reason.includes('PCM'), '错误信息指出不是 PCM', r.json.audio.reason);
}

// ===========================================================================
// 4. 阶段路由
// ===========================================================================

section('4. 四阶段路由');

{
  const env = makeEnv({ STAGE: 'mock' });
  const r = await post(makeWav({ seconds: 1 }), env);

  eq(r.status, 200, 'mock 阶段返回 200');
  eq(r.json.stage, 'mock', 'stage = mock');
  eq(env.aiCalls.length, 0, 'mock 阶段不调用任何 AI（硬件没到货时靠它开发 ESP32 端）');
  check(r.json.music !== undefined, 'mock 也返回完整 music 字段');
  check(r.json.music.chord_notes.length > 0, 'mock 的和弦音高非空');
  check(Array.isArray(r.json.music.melody_notes), 'mock 的旋律是数组');
  eq(r.json.labels.chords, ['Cmaj7', 'Am7', 'Dm7', 'G7'], 'mock 返回固定的 I-vi-ii-V');
  eq(r.json.music.bpm, 92, 'mock bpm = 92');
}

{
  const env = makeEnv({ STAGE: 'asr' });
  const r = await post(makeWav({ seconds: 1 }), env);

  eq(r.status, 200, 'asr 阶段返回 200');
  eq(env.aiCalls.length, 1, 'asr 阶段只调用一次 AI');
  check(env.aiCalls[0].model.includes('whisper'), 'asr 调用的是 whisper 模型', env.aiCalls[0].model);
  eq(r.json.text, '给我来一首欢快的音乐', '识别文本透传正确');
  check(r.json.music === undefined, 'asr 阶段不返回音乐数据（职责单一）');
  check(typeof env.aiCalls[0].input.audio === 'string', 'Whisper 的 audio 参数是 base64 字符串');
  check(env.aiCalls[0].input.audio.length > 100, 'base64 字符串非空');
}

{
  // 验证 base64 是合法的、且解出来就是原始 WAV 字节
  const env = makeEnv({ STAGE: 'asr' });
  const wav = makeWav({ seconds: 0.5 });
  await post(wav, env);
  const b64 = env.aiCalls[0].input.audio;
  const decoded = Buffer.from(b64, 'base64');
  eq(decoded.length, wav.byteLength, 'base64 解码后长度与原始 WAV 一致');
  const orig = new Uint8Array(wav);
  let same = true;
  for (let i = 0; i < orig.length; i++) {
    if (decoded[i] !== orig[i]) {
      same = false;
      break;
    }
  }
  check(same, 'base64 编解码是无损的（字节级一致）');
}

{
  const env = makeEnv({ STAGE: 'full' });
  const r = await post(makeWav({ seconds: 1 }), env);

  eq(r.status, 200, 'full 阶段返回 200');
  eq(env.aiCalls.length, 2, 'full 阶段调用两次 AI（whisper + llm）');
  check(env.aiCalls[0].model.includes('whisper'), '第一次调用 whisper');
  check(env.aiCalls[1].model.includes('llama'), '第二次调用 llama', env.aiCalls[1].model);

  eq(r.json.text, '给我来一首欢快的音乐', 'full 阶段返回识别文本');
  eq(r.json.music.key_index, 7, 'LLM 给的 key=G 被解析成 key_index 7');
  eq(r.json.music.bpm, 88, 'LLM 给的 bpm 生效');
  eq(r.json.labels.chords, ['Gmaj7', 'Em7', 'Am7', 'D7'], 'LLM 给的和弦走向生效');
  check(r.json.intent !== undefined, '同时返回 LLM 原始输出，便于调试');
  check(typeof r.json.timing.asr_ms === 'number', 'timing.asr_ms 存在');
  check(typeof r.json.timing.llm_ms === 'number', 'timing.llm_ms 存在');
  check(r.json.timing.total_ms >= 0, 'timing.total_ms 存在');
}

{
  // ★★★ 回归测试：真实踩过的坑 ★★★
  //
  // Cloudflare Workers AI 会把模型输出的 JSON 直接解析成【对象】返回，
  // 而不是返回字符串。最初的代码写的是 extractJson(res.response)，
  // 内部 String(对象) 得到字面量 "[object Object]" —— JSON.parse 必然失败，
  // 然后静默走了兜底，表现为「识别文本正确，但音乐永远是同一首」。
  //
  // 这个 bug 在 151 项断言下活了下来，因为桩数据一直返回的是字符串。
  // 现在把「response 是对象」这种情况也纳入测试，防止再次退化。
  const env = makeEnv({ STAGE: 'full' });
  env.AI.run = async (model) => {
    if (String(model).includes('whisper')) return { text: '来一首摇滚' };
    // 注意：这里是对象，不是字符串 —— 这就是真实 API 的行为
    return {
      response: {
        style: 'rock',
        instrument: 'clean_electric',
        bpm: 140,
        key: 'E',
        scale: 'major',
        bars: 4,
        chords: ['E', 'A', 'B7'],
        arp: 'arp_wave',
      },
    };
  };
  const r = await post(makeWav({ seconds: 1 }), env);
  eq(r.status, 200, '[回归] AI 返回对象时仍然 200');
  eq(r.json.intent_source, 'llm', '[回归] AI 返回对象时应判定为 llm，而不是 fallback');
  eq(r.json.music.bpm, 140, '[回归] AI 返回对象时 bpm 被正确读取');
  eq(r.json.music.key_index, 4, '[回归] key=E -> key_index 4');
  // labels.chords 给的是「和声素材」，不是每小节展开后的列表
  eq(r.json.labels.chords, ['E', 'A', 'B7'], '[回归] labels.chords 给的是和声素材');
  eq(r.json.music.bars, 16, '[回归] 未指定曲长时默认 16 小节');
  eq(r.json.labels.style, 'rock', '[回归] style 被正确读取');
}

{
  // LLM 乱输出时必须兜底，不能 500
  const env = makeEnv({ STAGE: 'full' });
  env.AI.run = async (model) => {
    if (String(model).includes('whisper')) return { text: '随便说点什么' };
    return { response: '对不起，我不太明白你的意思。' }; // 完全没有 JSON
  };
  const r = await post(makeWav({ seconds: 1 }), env);
  eq(r.status, 200, 'LLM 输出无法解析时仍然返回 200');
  eq(r.json.intent_source, 'fallback', 'LLM 输出无法解析时必须标记为 fallback（不能静默）');
  check(
    typeof r.json.intent_error === 'string' && r.json.intent_error.length > 0,
    'fallback 时必须给出 intent_error 说明原因',
    JSON.stringify(r.json.intent_error)
  );
  check(r.json.music !== undefined, '兜底后依然返回可用的 music 字段');
  eq(r.json.music.bpm, 90, '兜底 bpm = 90');
  eq(r.json.labels.chords.length, 4, '兜底和弦走向为 4 小节');
}

{
  // AI 抛异常时应该返回 502 而不是崩溃
  const env = makeEnv({ STAGE: 'asr' });
  env.AI.run = async () => {
    throw new Error('模型暂时不可用');
  };
  const r = await post(makeWav({ seconds: 1 }), env);
  eq(r.status, 502, 'AI 异常时返回 502');
  check(r.json.error.includes('asr failed'), '错误信息标明是 asr 阶段失败');
}

{
  // full 阶段收到非法 WAV 应该在调 AI 之前就拦住
  const env = makeEnv({ STAGE: 'full' });
  const r = await post(new Uint8Array(100).buffer, env);
  eq(r.status, 400, 'full 阶段非法 WAV 返回 400');
  eq(env.aiCalls.length, 0, '非法 WAV 时一次 AI 都不调用（省钱且快速失败）');
}

// ===========================================================================
// 5. 鉴权与限额
// ===========================================================================

section('5. 鉴权与限额');

{
  const env = makeEnv({ STAGE: 'echo', DEVICE_TOKEN: 'secret123' });
  const noAuth = await post(makeWav({ seconds: 1 }), env);
  eq(noAuth.status, 401, '配置了 token 但没带 -> 401');

  const wrong = await post(makeWav({ seconds: 1 }), env, { Authorization: 'Bearer wrong' });
  eq(wrong.status, 401, 'token 错误 -> 401');

  const right = await post(makeWav({ seconds: 1 }), env, { Authorization: 'Bearer secret123' });
  eq(right.status, 200, 'token 正确 -> 200');

  const health = await get('/api/v1/health', env);
  eq(health.json.auth_required, true, 'health 里报告需要鉴权');
}

{
  const env = makeEnv({ STAGE: 'echo', DEVICE_TOKEN: '' });
  const r = await post(makeWav({ seconds: 1 }), env);
  eq(r.status, 200, 'token 留空时不校验（开发期方便）');
  const health = await get('/api/v1/health', env);
  eq(health.json.auth_required, false, 'health 里报告不需要鉴权');
}

{
  const env = makeEnv({ STAGE: 'echo', MAX_AUDIO_SECONDS: '3' });
  const r = await post(makeWav({ seconds: 5 }), env);
  eq(r.status, 413, '超过 MAX_AUDIO_SECONDS -> 413');
  check(r.json.error.includes('too long'), '错误信息说明音频过长');
  eq(env.aiCalls.length, 0, '超长音频不浪费 AI 调用');
}

// ===========================================================================
// 6. HTTP 路由
// ===========================================================================

section('6. HTTP 路由');

{
  const env = makeEnv({ STAGE: 'mock' });

  const health = await get('/api/v1/health', env);
  eq(health.status, 200, 'GET /api/v1/health -> 200');
  eq(health.json.service, 'music-intent-api', 'health 返回服务名');
  eq(health.json.stage, 'mock', 'health 返回当前 stage');

  const root = await get('/', env);
  eq(root.status, 200, 'GET / -> 200');
  check(root.contentType.includes('text/html'), 'GET / 返回 HTML');
  check(root.text.includes('Karplus') || root.text.includes('音乐生成'), '测试页包含预期内容');

  const notFound = await get('/nope', env);
  eq(notFound.status, 404, '未知路径 -> 404');

  const wrongMethod = await worker.fetch(new Request('https://test.local/api/v1/intent'), env);
  eq(wrongMethod.status, 405, 'GET /api/v1/intent -> 405（只接受 POST）');

  const preflight = await worker.fetch(
    new Request('https://test.local/api/v1/intent', { method: 'OPTIONS' }),
    env
  );
  eq(preflight.status, 204, 'OPTIONS 预检 -> 204');
  check(
    preflight.headers.get('Access-Control-Allow-Origin') === '*',
    'CORS 头存在'
  );
}

// ===========================================================================
// 7. 测试页内联脚本的语法正确性
// ===========================================================================

section('7. 测试页内联脚本');

{
  // 内联脚本在外层模板字符串里，反斜杠和转义很容易写错。
  // 这里把 <script> 内容抠出来交给 JS 引擎解析，能立刻发现转义问题。
  const m = /<script>([\s\S]*?)<\/script>/.exec(DEMO_PAGE);
  check(m !== null, '测试页里有 <script> 块');

  if (m) {
    let syntaxOk = true;
    let syntaxErr = '';
    try {
      // eslint-disable-next-line no-new-func
      new Function(m[1]);
    } catch (e) {
      syntaxOk = false;
      syntaxErr = e.message;
    }
    check(syntaxOk, '内联脚本语法正确（转义没有写错）', syntaxErr);

    check(m[1].includes('ksRender'), '测试页包含 Karplus-Strong 合成器');
    check(m[1].includes('OfflineAudioContext'), '测试页用 OfflineAudioContext 重采样到 16kHz');
    check(m[1].includes('encodeWav'), '测试页包含 WAV 编码器');
    check(m[1].includes('Authorization'), '测试页会把 DEVICE_TOKEN 放进请求头');
    check(
      m[1].includes('echoCancellation: false'),
      '测试页关掉了浏览器音效处理（否则测的不是真实麦克风）'
    );
  }

  check(DEMO_PAGE.includes('16kHz'), '测试页说明了 16kHz 契约');
}

// ===========================================================================
// 汇总
// ===========================================================================

console.log('\n' + '='.repeat(64));
if (failed === 0) {
  console.log(`✅ 全部通过：${passed} 项断言`);
  process.exit(0);
} else {
  console.log(`❌ 失败 ${failed} 项 / 共 ${passed + failed} 项`);
  console.log('\n失败明细：');
  failures.forEach((f) => console.log(`  - ${f.label}${f.detail ? `\n      ${f.detail}` : ''}`));
  process.exit(1);
}
