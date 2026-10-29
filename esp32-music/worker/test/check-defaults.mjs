/**
 * 检查线上页面的滑块默认值
 * ===========================================================================
 * 用脚本文件而不是 node -e 内联脚本：内联时 PowerShell 的引号转义会把命令解析坏，
 * 而且中文还会被 GBK 编码搞乱。
 *
 * 用法：
 *   $env:NODE_USE_ENV_PROXY='1'; $env:HTTPS_PROXY='http://127.0.0.1:7897'
 *   node test/check-defaults.mjs
 */

const BASE = process.env.WORKER_URL || 'https://music-intent-api.2356766287.workers.dev';

// 期望值 = 在测试台上听感调出来的那一组
// 注意：这里用数字而不是字符串比较。"0.8" 和 "0.80" 在 HTML 里写法不同但数值相同，
// 按字符串比会产生假失败（踩过一次）。
const EXPECTED = {
  sRev: 0.16, // 混响
  sDecay: 2.0, // 衰减
  sBright: 0.6, // 亮度
  sOverlap: 0, // 叠音（0=关）
  sDrum: 0.8, // 鼓点
  sDamp: 0.38, // 阻尼
  sPick: 0.4, // 拨弦位
  sLoop: 1, // 循环（1=开）
};

const NAMES = {
  sRev: '混响',
  sDecay: '衰减',
  sBright: '亮度',
  sOverlap: '叠音',
  sDrum: '鼓点',
  sDamp: '阻尼',
  sPick: '拨弦位',
  sLoop: '循环',
};

// 加缓存破坏参数：Workers 的响应可能被中间环节（代理、边缘缓存）缓存住，
// 不加的话会拿到旧版本页面，产生莫名其妙的假失败。
const res = await fetch(BASE + '/?cb=' + Date.now());
const html = await res.text();
if (res.status !== 200) {
  console.log(`❌ 页面请求失败 HTTP ${res.status}`);
  process.exitCode = 1;
} else {
  console.log('='.repeat(62));
  console.log('线上页面滑块默认值');
  console.log('='.repeat(62));
  console.log('滑块        期望      实际      结果');
  console.log('-'.repeat(62));

  let pass = 0;
  for (const [id, want] of Object.entries(EXPECTED)) {
    // 在 id="sXxx" 之后找同标签内的 value="..."
    const re = new RegExp(`id="${id}"[^>]*?value="([^"]*)"`);
    const m = re.exec(html);
    const raw = m ? m[1] : '';
    const got = raw === '' ? NaN : parseFloat(raw);
    const ok = Number.isFinite(got) && Math.abs(got - want) < 1e-9;
    if (ok) pass++;
    console.log(
      `${NAMES[id].padEnd(8)}  ${String(want).padEnd(8)}  ${(m ? raw : '(未找到)').padEnd(8)}  ${ok ? '✓' : '✗'}`
    );
    if (!ok && m) {
      // 把整段 tag 打出来，方便看清到底是哪个属性没对上
      const tagStart = html.lastIndexOf('<', m.index);
      const tagEnd = html.indexOf('>', m.index);
      console.log(`          实际标签: ${html.slice(tagStart, tagEnd + 1)}`);
    }
  }

  console.log('-'.repeat(62));
  const total = Object.keys(EXPECTED).length;
  console.log(pass === total ? `✅ 全部一致 (${pass}/${total})` : `❌ ${pass}/${total} 不一致`);
  process.exitCode = pass === total ? 0 : 1;
}
