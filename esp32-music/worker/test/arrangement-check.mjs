/**
 * 编排结构与响应体积检查
 * ===========================================================================
 *
 * 检查两件事：
 *   1) 曲式结构（段落划分、旋律的动机重复、鼓点动态）是不是真的生效了
 *   2) 响应体积有多大 —— 这直接决定 ESP32 端接收缓冲要开多大
 *
 * 用法：
 *   $env:NODE_USE_ENV_PROXY='1'; $env:HTTPS_PROXY='http://127.0.0.1:7897'
 *   node test/arrangement-check.mjs
 */

const BASE = process.env.WORKER_URL || 'https://music-intent-api.2356766287.workers.dev';

function makeWav(seconds = 2, sampleRate = 16000) {
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
    const s = 0.2 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    dv.setInt16(44 + i * 2, Math.round(s * 32767), true);
  }
  return buf;
}

const res = await fetch(`${BASE}/api/v1/intent`, {
  method: 'POST',
  headers: { 'Content-Type': 'audio/wav' },
  body: makeWav(2),
});

const raw = await res.text();
const body = JSON.parse(raw);
const m = body.music;

if (!m) {
  console.log('❌ 响应里没有 music 字段');
  console.log(JSON.stringify(body, null, 2).slice(0, 800));
  process.exit(1);
}

const bytes = Buffer.byteLength(raw);
const prettyBytes = Buffer.byteLength(JSON.stringify(body, null, 2));
const compactBytes = Buffer.byteLength(JSON.stringify(body));

console.log('='.repeat(76));
console.log('响应体积（决定固件端接收缓冲要开多大）');
console.log('='.repeat(76));
console.log(`  美化输出（当前线上格式）  : ${prettyBytes} 字节  (${(prettyBytes / 1024).toFixed(1)} KB)`);
console.log(`  紧凑输出（无缩进）        : ${compactBytes} 字节  (${(compactBytes / 1024).toFixed(1)} KB)`);
console.log();
console.log(`  建议固件端接收缓冲        : >= ${Math.ceil((prettyBytes * 1.6) / 1024)} KB`);
console.log(`  （留 60% 余量，因为句子长度不同响应大小会浮动）`);

console.log();
console.log('='.repeat(76));
console.log('曲式结构');
console.log('='.repeat(76));
console.log(`  意图来源: ${body.intent_source}   调式: ${body.labels.key} ${body.labels.scale}   速度: ${m.bpm} BPM`);
console.log(`  和声素材: ${body.labels.chords.join(' - ')}`);
console.log(`  曲式    : ${body.labels.form}`);
console.log(`  总长    : ${m.bars} 小节  (${m.steps_per_beat} 格/拍 → 每小节 ${m.beats_per_bar * m.steps_per_beat} 格)`);
const totalSec = m.bars * m.beats_per_bar * (m.step_ms / 1000);
console.log(`  时长    : 约 ${totalSec.toFixed(1)} 秒`);
console.log();

console.log('='.repeat(76));
console.log('逐小节内容（看动机是否重复、段落是否有对比）');
console.log('='.repeat(76));

const spb = m.beats_per_bar * m.steps_per_beat;
const secLabels = [];
for (const s of m.sections || []) {
  for (let i = 0; i < s.bars; i++) secLabels.push(s.name);
}

for (let b = 0; b < m.bars; b++) {
  const seg = m.melody_notes.slice(b * spb, (b + 1) * spb);
  const notes = seg.map((n) => (n < 0 ? '  .' : String(n).padStart(3))).join('');
  const lvl = m.drum_level ? m.drum_level[b] : '?';
  console.log(
    `  ${String(b).padStart(2)} [${(secLabels[b] || '?').padEnd(6)}] 鼓${String(lvl).padStart(2)}  ${notes}`
  );
}

console.log();
console.log('='.repeat(76));
console.log('自检');
console.log('='.repeat(76));

const checks = [];
const ok = (cond, label) => checks.push({ cond, label });

ok(m.drum_level && m.drum_level.length === m.bars, '鼓点动态数组长度等于曲长');
ok(m.bass_notes && m.bass_notes.length === m.bars, '低音数组长度等于曲长');
ok(m.chord_note_counts.length === m.bars, '和弦小节数等于曲长');
ok(m.melody_notes.length === m.bars * spb, '旋律长度 = 曲长 × 每小节格数');
ok((m.sections || []).length >= 2, '至少有两个段落（有曲式结构）');

// 动机重复：同一段落内的小节节奏应当大量重合
if (m.sections && m.sections.length >= 2) {
  const sec = m.sections.find((s) => s.name === 'verse') || m.sections[1];
  let start = 0;
  for (const s of m.sections) {
    if (s === sec) break;
    start += s.bars;
  }
  const rhythm = (b) =>
    m.melody_notes.slice(b * spb, (b + 1) * spb).map((n) => (n >= 0 ? 1 : 0)).join('');
  if (sec.bars >= 4) {
    const r0 = rhythm(start);
    const r2 = rhythm(start + 2);
    ok(r0 === r2, `动机重复：${sec.name} 段第 1、3 小节节奏一致 (${r0} vs ${r2})`);
  }

  // 段落对比：副歌的平均音高应高于主歌
  const avg = (from, n) => {
    const seg = m.melody_notes
      .slice(from * spb, (from + n) * spb)
      .filter((x) => x >= 0);
    return seg.length ? seg.reduce((a, b) => a + b, 0) / seg.length : 0;
  };
  let verseStart = 0;
  for (const s of m.sections) {
    if (s.name === 'verse') break;
    verseStart += s.bars;
  }
  const verse = m.sections.find((s) => s.name === 'verse');
  const chorus = m.sections.find((s) => s.name === 'chorus');
  if (verse && chorus) {
    const va = avg(verseStart, verse.bars);
    const ca = avg(verseStart + verse.bars, chorus.bars);
    ok(ca > va + 6, `副歌比主歌高八度（主歌均音高 ${va.toFixed(1)} → 副歌 ${ca.toFixed(1)}）`);
  }
}

let pass = 0;
for (const c of checks) {
  console.log(`  ${c.cond ? '✓' : '✗'} ${c.label}`);
  if (c.cond) pass++;
}
console.log();
console.log(pass === checks.length ? `✅ 全部通过 (${pass}/${checks.length})` : `❌ ${pass}/${checks.length}`);
// 用 exitCode 而不是 process.exit()：Windows 上 fetch 还有未关闭的句柄时
// 强行 process.exit() 会触发 libuv 的 UV_HANDLE_CLOSING 断言崩溃
process.exitCode = pass === checks.length ? 0 : 1;
