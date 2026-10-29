/**
 * 生成样例响应文件
 * ===========================================================================
 *
 * 直接从真实代码跑出一份响应写进 docs/sample-response.json。
 *
 * 为什么要有这个：手写的文档示例一定会和代码脱节。
 * 从代码生成可以保证固件端看到的字段名、类型、数组结构都是真的。
 *
 * 跑法：node test/sample.mjs
 */

import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import worker from '../src/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const outPath = join(here, '..', '..', 'docs', 'sample-response.json');

/** 生成一段 5 秒 16kHz 单声道 16bit WAV（内容为一段和弦感的正弦叠加，仅供演示） */
function makeWav(seconds = 5, sampleRate = 16000) {
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
    // 三个音叠加，模拟有人在说话（有能量但不削顶）
    const s =
      0.12 * Math.sin(2 * Math.PI * 196 * t) +
      0.09 * Math.sin(2 * Math.PI * 247 * t) +
      0.06 * Math.sin(2 * Math.PI * 330 * t) +
      0.04 * Math.sin(2 * Math.PI * 660 * t);
    dv.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, s)) * 32767), true);
  }
  return buf;
}

// 桩 AI：输出符合真实风格的 LLM 响应（带 markdown 围栏，验证后端的容错解析）
const env = {
  STAGE: 'full',
  DEVICE_TOKEN: 'demo-token-abc123',
  MAX_AUDIO_SECONDS: '15',
  AI: {
    async run(model) {
      if (String(model).includes('whisper')) {
        return { text: '给我来一首欢快的音乐' };
      }
      return {
        response:
          '```json\n' +
          JSON.stringify(
            {
              style: 'mandopop',
              instrument: 'nylon_guitar',
              bpm: 88,
              key: 'G',
              scale: 'major',
              bars: 4,
              chords: ['Gmaj7', 'Em7', 'Am7', 'D7'],
              arp: 'fingerpick',
            },
            null,
            2
          ) +
          '\n```',
      };
    },
  },
};

const res = await worker.fetch(
  new Request('https://music-intent-api.example.workers.dev/api/v1/intent', {
    method: 'POST',
    headers: {
      'Content-Type': 'audio/wav',
      Authorization: 'Bearer demo-token-abc123',
    },
    body: makeWav(5),
  }),
  env
);

const body = await res.json();
body.timing = { asr_ms: 1840, llm_ms: 920, total_ms: 3010 };

writeFileSync(outPath, JSON.stringify(body, null, 2) + '\n', 'utf8');

const m = body.music;
console.log(`已写入 ${outPath}`);
console.log(`  HTTP           ${res.status}`);
console.log(`  text           "${body.text}"`);
console.log(`  bpm / key      ${m.bpm} / ${body.labels.key} ${body.labels.scale}`);
console.log(`  chords         ${body.labels.chords.join(' - ')}`);
console.log(`  chord_notes    [${m.chord_notes.join(', ')}]`);
console.log(`  melody_notes   [${m.melody_notes.join(', ')}]`);
console.log(`  step_ms        ${m.step_ms}`);
console.log(`  响应体积       ${Buffer.byteLength(JSON.stringify(body))} 字节`);
