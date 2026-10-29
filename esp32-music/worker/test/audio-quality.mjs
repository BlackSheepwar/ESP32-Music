/**
 * 音质诊断：先算，再改
 * ===========================================================================
 *
 * 在动手改算法之前，先用数字确认「到底哪里难听」。
 *
 * 头号嫌疑：Karplus-Strong 的延迟线长度 N = 采样率 / 频率，
 * 而 N 必须是整数。取整会让每个音实际发出的频率偏离目标 —— 而且
 * 【不同音高偏离的程度不一样】，于是和弦内部互相走音。
 *
 * 这个脚本量化这个问题有多严重。
 *
 * 用法: node test/audio-quality.mjs
 */

const SR = 16000;
const A4 = 440;

const midiToFreq = (m) => A4 * Math.pow(2, (m - 69) / 12);
const cents = (actual, target) => 1200 * Math.log2(actual / target);

/** 当前实现：N 取整 */
function currentDelay(freq) {
  const n = Math.round(SR / freq);
  return Math.max(2, n);
}

/** 改进后：N 取整 + 分数部分用全通滤波补足 */
function fractionalEta(freq) {
  const exact = SR / freq;
  const n = Math.max(2, Math.floor(exact));
  const frac = exact - n;
  return { n, frac, eta: (1 - frac) / (1 + frac) };
}

console.log('='.repeat(78));
console.log('Karplus-Strong 延迟线取整带来的音准误差');
console.log(`采样率 ${SR} Hz`);
console.log('='.repeat(78));
console.log();
console.log('MIDI  音名     目标Hz    当前实现Hz   误差(音分)   误差(%)');
console.log('-'.repeat(78));

const NOTES = [
  [48, 'C3'], [52, 'E3'], [55, 'G3'],
  [57, 'A3'], [60, 'C4'], [62, 'D4'], [64, 'E4'], [65, 'F4'], [67, 'G4'], [69, 'A4'],
  [71, 'B4'], [72, 'C5'], [76, 'E5'], [79, 'G5'], [84, 'C6'],
];

const rows = [];
for (const [midi, name] of NOTES) {
  const target = midiToFreq(midi);
  const n = currentDelay(target);
  const actual = SR / n;
  const err = cents(actual, target);
  const pct = ((actual - target) / target) * 100;
  rows.push({ midi, name, target, actual, err, pct });
  console.log(
    `${String(midi).padStart(4)}  ${name.padEnd(6)}  ${target.toFixed(2).padStart(8)}  ` +
      `${actual.toFixed(2).padStart(10)}  ${(err >= 0 ? '+' : '') + err.toFixed(1).padStart(8)}  ` +
      `${(pct >= 0 ? '+' : '') + pct.toFixed(2).padStart(6)}%`
  );
}

console.log();
console.log('='.repeat(78));
console.log('★ 关键指标：一个和弦内部的"走音"范围');
console.log('='.repeat(78));
console.log();
console.log('人耳能察觉的音准偏差约 5-10 音分。超过 20 音分就会明显觉得"不准"。');
console.log();

/** 当前实现下，一个和弦里的各音高偏差范围 */
const CHORDS = {
  'Cmaj7 (C-E-G-B)': [48, 52, 55, 59],
  'Am7 (A-C-E-G)': [45, 48, 52, 55],
  'Dm7 (D-F-A-C)': [50, 53, 57, 60],
  'G7 (G-B-D-F)': [43, 47, 50, 53],
  'C4 单音': [60],
};

for (const [label, notes] of Object.entries(CHORDS)) {
  const errs = notes.map((m) => {
    const t = midiToFreq(m);
    return cents(SR / currentDelay(t), t);
  });
  const min = Math.min(...errs);
  const max = Math.max(...errs);
  const spread = max - min;
  const flag = spread > 15 ? '  ❌ 明显走音' : spread > 8 ? '  ⚠️ 可察觉' : '  ✅ 可接受';
  console.log(
    `${label.padEnd(20)} 误差范围 ${min.toFixed(1).padStart(7)} ~ ${max.toFixed(1).padStart(6)} 音分` +
      `   跨度 ${spread.toFixed(1).padStart(5)} 音分${flag}`
  );
}

console.log();
console.log('='.repeat(78));
console.log('修复方案验证：整数延迟 + 全通分数延迟');
console.log('='.repeat(78));
console.log();
console.log('做法：N 取 floor，剩下的小数部分 frac 用一阶全通滤波器补足。');
console.log('      全通系数 eta = (1 - frac) / (1 + frac)');
console.log('      一阶全通在低频处的相位延迟 ≈ frac 个采样点，正好补上缺口。');
console.log();

// 全通滤波器的实际相位延迟（在给定频率处）
function allpassPhaseDelay(eta, freq, sr) {
  const w = (2 * Math.PI * freq) / sr;
  // H(z) = (eta + z^-1) / (1 + eta z^-1)
  // 相位延迟 = -arg(H)/w
  const numRe = eta + Math.cos(-w);
  const numIm = Math.sin(-w);
  const denRe = 1 + eta * Math.cos(-w);
  const denIm = eta * Math.sin(-w);
  const numPhase = Math.atan2(numIm, numRe);
  const denPhase = Math.atan2(denIm, denRe);
  const phase = numPhase - denPhase;
  return -phase / w;
}

console.log('MIDI  音名    总延迟(含分数)   实际Hz     误差(音分)');
console.log('-'.repeat(78));

let maxErr = 0;
for (const [midi, name] of NOTES) {
  const target = midiToFreq(midi);
  const { n, frac, eta } = fractionalEta(target);
  const apDelay = allpassPhaseDelay(eta, target, SR);
  const totalDelay = n + apDelay;
  const actual = SR / totalDelay;
  const err = cents(actual, target);
  maxErr = Math.max(maxErr, Math.abs(err));
  console.log(
    `${String(midi).padStart(4)}  ${name.padEnd(6)}  ${totalDelay.toFixed(4).padStart(14)}  ` +
      `${actual.toFixed(2).padStart(9)}  ${(err >= 0 ? '+' : '') + err.toFixed(2).padStart(9)}`
  );
}

console.log();
console.log('='.repeat(78));
console.log('★ 扩展 KS：环路里加了阻尼低通之后，音准还准吗？');
console.log('='.repeat(78));
console.log();
console.log('这是扩展 Karplus-Strong 最容易踩的坑：');
console.log('  环路里每加一个滤波器，都会带来额外的【相位延迟】。');
console.log('  如果不从延迟线长度里扣掉，总延迟就变长 -> 音高会偏低。');
console.log();
console.log('做法：先从 sr/freq 里减去低通的相位延迟，剩下的再拆成');
console.log('      「整数 N + 全通分数 frac」。这样总延迟才等于目标值。');
console.log();

/**
 * 完整模拟扩展 KS 的延迟计算，返回实际基频
 * damping = 0 时退化成"整数延迟 + 全通分数延迟"
 */
function eksActualFreq(freq, damping, sr) {
  const w = (2 * Math.PI * freq) / sr;
  const d = Math.max(0, Math.min(0.6, damping));
  // 一阶低通 (1-d)/(1 - d z^-1) 在频率 w 处的相位延迟
  const lpPhase = d > 0 ? Math.atan2(d * Math.sin(w), 1 - d * Math.cos(w)) / w : 0;

  let total = sr / freq - lpPhase;
  if (total < 2) total = 2;
  const N = Math.floor(total);
  const frac = total - N;
  const eta = (1 - frac) / (1 + frac);
  const apPhase = allpassPhaseDelay(eta, freq, sr);

  const realTotal = N + apPhase + lpPhase;
  return { freq: sr / realTotal, N, frac, lpPhase, apPhase, total: realTotal };
}

console.log('damping=0.30（尼龙弦吉他的值）下的音准：');
console.log();
console.log('MIDI  音名     目标Hz     实际Hz     误差(音分)');
console.log('-'.repeat(78));

let maxEksErr = 0;
for (const [midi, name] of NOTES) {
  const target = midiToFreq(midi);
  const r = eksActualFreq(target, 0.3, SR);
  const err = cents(r.freq, target);
  maxEksErr = Math.max(maxEksErr, Math.abs(err));
  console.log(
    `${String(midi).padStart(4)}  ${name.padEnd(6)}  ${target.toFixed(2).padStart(8)}  ` +
      `${r.freq.toFixed(2).padStart(9)}  ${(err >= 0 ? '+' : '') + err.toFixed(3).padStart(9)}`
  );
}

console.log();
console.log(`最大误差: ${maxEksErr.toFixed(3)} 音分`);

// 对照：如果不做相位补偿会怎样
console.log();
console.log('对照 —— 如果【忘了】从延迟线里扣掉低通的相位延迟：');
console.log();
console.log('MIDI  音名     目标Hz     实际Hz     误差(音分)');
console.log('-'.repeat(78));
let maxBadErr = 0;
for (const [midi, name] of [NOTES[0], NOTES[5], NOTES[9], NOTES[11], NOTES[14]]) {
  const target = midiToFreq(midi);
  const w = (2 * Math.PI * target) / SR;
  const d = 0.3;
  const lpPhase = Math.atan2(d * Math.sin(w), 1 - d * Math.cos(w)) / w;
  // 错误做法：直接从 sr/freq 取整，不管相位延迟
  const N = Math.max(2, Math.floor(SR / target));
  const frac = SR / target - N;
  const eta = (1 - frac) / (1 + frac);
  const apPhase = allpassPhaseDelay(eta, target, SR);
  const badTotal = N + apPhase + lpPhase;
  const badFreq = SR / badTotal;
  const err = cents(badFreq, target);
  maxBadErr = Math.max(maxBadErr, Math.abs(err));
  console.log(
    `${String(midi).padStart(4)}  ${name.padEnd(6)}  ${target.toFixed(2).padStart(8)}  ` +
      `${badFreq.toFixed(2).padStart(9)}  ${(err >= 0 ? '+' : '') + err.toFixed(2).padStart(9)}`
  );
}
console.log();
console.log(`不补偿时的最大误差: ${maxBadErr.toFixed(2)} 音分 ${maxBadErr > 5 ? '❌ 明显走音' : '✅'}`);
console.log();
console.log('='.repeat(78));
console.log('总结');
console.log('='.repeat(78));
console.log();
console.log(`  原始实现（round 取整）      最大误差 ${Math.max(...rows.map((r) => Math.abs(r.err))).toFixed(2)} 音分  ❌`);
console.log(`  只加全通分数延迟            最大误差 ${maxErr.toFixed(2)} 音分  ✅`);
console.log(`  扩展 KS + 相位延迟补偿      最大误差 ${maxEksErr.toFixed(3)} 音分  ✅`);
console.log(`  扩展 KS 但不做相位补偿      最大误差 ${maxBadErr.toFixed(2)} 音分  ❌`);
console.log();
console.log('结论：扩展 KS 的滤波器可以加，但【必须】把它们的相位延迟');
console.log('      从延迟线长度里扣掉，否则音准会重新变差。');
