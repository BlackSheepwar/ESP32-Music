/**
 * 意图解析调试工具
 * ===========================================================================
 *
 * 用【真实的系统提示词】跑一次 LLM 意图解析，把模型原始输出原样打印出来。
 *
 * 为什么需要它：
 *   之前的排查路径是「录音 → 上传 → 看响应」，每轮要几十秒，而且还夹着
 *   Whisper 这一层干扰。这个工具跳过录音和 ASR，直接打 LLM 这一环，
 *   一轮只要几百毫秒。
 *
 * 用法：
 *   node test/intent-debug.mjs
 *   node test/intent-debug.mjs "来一首摇滚"
 *   node test/intent-debug.mjs "来一首摇滚" "@cf/meta/llama-3.3-70b-instruct-fp8-fast"
 *
 * 注意：中国大陆访问 workers.dev 需要走代理，先设置：
 *   $env:NODE_USE_ENV_PROXY='1'; $env:HTTPS_PROXY='http://127.0.0.1:7897'
 */

const BASE = process.env.WORKER_URL || 'https://music-intent-api.2356766287.workers.dev';
const text = process.argv[2] || '给我来一首欢快的音乐';
const model = process.argv[3] || '';

const url =
  `${BASE}/api/v1/aitest?intent=${encodeURIComponent(text)}` +
  (model ? `&model=${encodeURIComponent(model)}` : '');

console.log(`目标   : ${BASE}`);
console.log(`输入   : ${text}`);
console.log(`模型   : ${model || '(默认)'}`);
console.log('='.repeat(70));

let r;
try {
  r = await fetch(url);
} catch (e) {
  console.log(`\n❌ 请求失败: ${e.message}`);
  console.log('\n如果是在中国大陆，先设置代理：');
  console.log("  $env:NODE_USE_ENV_PROXY='1'; $env:HTTPS_PROXY='http://127.0.0.1:7897'");
  process.exit(1);
}

const j = await r.json();

console.log(`HTTP       : ${r.status}`);
console.log(`模型       : ${j.model}`);
console.log(`耗时       : ${j.ms} ms`);
console.log(`★ 返回类型   : ${j.response_type ?? '(未报告)'}`);
console.log(`JSON 可解析 : ${j.parsed_ok ? '✅ 是' : '❌ 否'}`);
console.log(`原始长度   : ${j.raw_length ?? '(无)'}`);

if (j.error) {
  console.log(`\n错误: ${j.error}`);
}

if (j.raw) {
  console.log('\n' + '-'.repeat(70));
  console.log('模型原始输出：');
  console.log('-'.repeat(70));
  console.log(j.raw);
  console.log('-'.repeat(70));
}

if (j.parsed) {
  console.log('\n解析出的意图：');
  console.log(JSON.stringify(j.parsed, null, 2));
} else {
  console.log('\n⚠️ 解析失败 —— 这就是「音乐永远一样」的根因。');
  console.log('   上面那段原始输出就是模型真正吐的东西，照它来修提示词。');
}
