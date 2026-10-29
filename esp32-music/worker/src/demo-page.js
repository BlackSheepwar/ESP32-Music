/**
 * 浏览器录音测试页（内联在 Worker 里，随 Worker 一起部署）
 * ===========================================================================
 *
 * 为什么需要它：硬件没到货时，这是唯一能做的端到端验证。
 *
 * 关键设计：这个页面录出来的音频**和 ESP32 将要发送的完全一致** ——
 *   16kHz / 单声道 / 16bit PCM / 带 44 字节 WAV 头
 * 所以它验证的是真实契约，不是近似。
 *
 * 附带一个 JS 版的 Karplus-Strong 合成器：
 *   用后端返回的 music 字段（decay_seconds / brightness / chord_notes /
 *   arp_pattern / melody_notes）实时渲染播放。
 *   好处是——你可以在 ESP32 到货之前就把合成参数调到好听，
 *   板子到了以后直接把这套参数搬到 C 代码里。
 *
 * 实现细节：
 *   录音用 MediaRecorder 拿到压缩音频 -> decodeAudioData 解码
 *   -> OfflineAudioContext 重采样到 16kHz -> 手动编码 WAV。
 *   这条路只用了稳定的浏览器 API，不需要 ScriptProcessorNode / AudioWorklet。
 */

export const DEMO_PAGE = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 音乐生成 - 云端接口测试台</title>
<style>
  :root {
    --bg: #0f1115; --panel: #171a21; --line: #262b36;
    --fg: #e6e9ef; --dim: #8b93a5; --accent: #4f8cff; --ok: #35c88a; --warn: #f0a441; --err: #f05a5a;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--fg);
         font: 14px/1.6 ui-sans-serif, system-ui, "Segoe UI", "Microsoft YaHei", sans-serif; }
  .wrap { max-width: 980px; margin: 0 auto; padding: 28px 20px 80px; }
  h1 { font-size: 20px; margin: 0 0 4px; }
  h2 { font-size: 14px; margin: 24px 0 10px; color: var(--dim);
       text-transform: uppercase; letter-spacing: .08em; font-weight: 600; }
  .sub { color: var(--dim); font-size: 13px; margin-bottom: 20px; }
  .panel { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 16px; }
  .row { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
  button { background: var(--accent); color: #fff; border: 0; border-radius: 8px;
           padding: 10px 18px; font-size: 14px; font-weight: 600; cursor: pointer; }
  button:disabled { opacity: .4; cursor: not-allowed; }
  button.ghost { background: transparent; border: 1px solid var(--line); color: var(--fg); font-weight: 500; }
  button.rec { background: var(--err); }
  input[type=text] { background: #0c0e12; border: 1px solid var(--line); color: var(--fg);
                     border-radius: 8px; padding: 9px 12px; font-size: 13px; min-width: 240px; }
  .badge { display: inline-block; padding: 3px 10px; border-radius: 999px; font-size: 12px;
           font-weight: 600; background: #23303f; color: var(--accent); }
  .badge.ok { background: #123326; color: var(--ok); }
  .badge.warn { background: #3a2d13; color: var(--warn); }
  .kv { display: grid; grid-template-columns: 150px 1fr; gap: 4px 14px; font-size: 13px; }
  .kv dt { color: var(--dim); }
  .kv dd { margin: 0; font-variant-numeric: tabular-nums; }
  .hint { font-size: 12.5px; color: var(--warn); margin: 4px 0; padding-left: 16px; position: relative; }
  .hint::before { content: "\\2192"; position: absolute; left: 0; }
  .hint.ok { color: var(--ok); }
  .transcript { font-size: 17px; padding: 12px 14px; background: #0c0e12;
                border: 1px solid var(--line); border-radius: 8px; min-height: 44px; }
  .chords { display: flex; gap: 8px; flex-wrap: wrap; margin: 8px 0 0; }
  .chip { background: #1e2635; border: 1px solid var(--line); border-radius: 6px;
          padding: 5px 12px; font-weight: 600; font-size: 14px; }
  canvas { width: 100%; height: 150px; background: #0c0e12; border: 1px solid var(--line); border-radius: 8px; }
  details { margin-top: 12px; }
  summary { cursor: pointer; color: var(--dim); font-size: 13px; }
  pre { background: #0c0e12; border: 1px solid var(--line); border-radius: 8px;
        padding: 12px; overflow: auto; font-size: 12px; max-height: 380px; }
  .log { font-family: ui-monospace, Consolas, monospace; font-size: 12px;
         color: var(--dim); white-space: pre-wrap; max-height: 130px; overflow: auto; }
  .hidden { display: none; }
  .status { font-size: 13px; color: var(--dim); }
  .slider { display: inline-flex; align-items: center; gap: 8px; font-size: 12.5px; color: var(--dim); }
  .slider input[type=range] { width: 110px; accent-color: var(--accent); }
  .slider b { color: var(--fg); font-variant-numeric: tabular-nums; min-width: 34px; display: inline-block; }
</style>
</head>
<body>
<div class="wrap">
  <h1>ESP32 音乐生成 &mdash; 云端接口测试台</h1>
  <div class="sub">
    录一段话（比如「给我来一首欢快的音乐」），上传到 Worker，看返回的结构化音乐数据，并直接试听。
    录制的音频格式与 ESP32 端完全一致：16kHz / 单声道 / 16bit WAV。
  </div>

  <div class="panel">
    <div class="row">
      <button id="btnRec">&#9679; 开始录音</button>
      <button id="btnPlay" class="ghost" disabled>&#9654; 试听生成的音乐</button>
      <button id="btnStop" class="ghost" disabled>&#9632; 停止播放</button>
      <input type="text" id="token" placeholder="DEVICE_TOKEN（未配置可留空）">
      <span id="stage" class="badge">加载中</span>
    </div>
    <div class="row" style="margin-top:12px">
      <span id="status" class="status">就绪</span>
    </div>
    <div class="row" style="margin-top:14px;gap:22px">
      <label class="slider">混响
        <input type="range" id="sRev" min="0" max="0.7" step="0.02" value="0.16">
        <b id="vRev">0.16</b>
      </label>
      <label class="slider">衰减
        <input type="range" id="sDecay" min="0.4" max="8" step="0.1" value="2.0">
        <b id="vDecay">2.0</b>s
      </label>
      <label class="slider">亮度
        <input type="range" id="sBright" min="0.05" max="0.95" step="0.05" value="0.60">
        <b id="vBright">0.60</b>
      </label>
      <label class="slider">叠音
        <input type="range" id="sOverlap" min="0" max="1" step="1" value="0">
        <b id="vOverlap">关</b>
      </label>
      <label class="slider">鼓点
        <input type="range" id="sDrum" min="0" max="1.2" step="0.05" value="0.8">
        <b id="vDrum">0.80</b>
      </label>
      <label class="slider">阻尼
        <input type="range" id="sDamp" min="0" max="0.6" step="0.02" value="0.38">
        <b id="vDamp">0.38</b>
      </label>
      <label class="slider">拨弦位
        <input type="range" id="sPick" min="0" max="0.5" step="0.02" value="0.40">
        <b id="vPick">0.40</b>
      </label>
      <label class="slider">循环
        <input type="range" id="sLoop" min="0" max="1" step="1" value="1">
        <b id="vLoop">开</b>
      </label>
    </div>
    <div class="sub" style="margin:10px 0 0;font-size:12px">
      七个滑块可以实时调整音色，改完直接点【试听】。找到好听的值告诉我，我把它写进默认参数。<br>
      「叠音」= 关时同一个音高再次触发会掐掉上一个（更干净）；开时允许同时响（更厚但可能嗡）。<br>
      「阻尼」= 扩展 KS 的环路阻尼：控制高频消失得多快。低音能嗡很久但高音很快只剩基频，真实弦就是这样。<br>
      「拨弦位」= 在离琴桥这个比例的位置拨弦，产生谐波陷波。小=靠近琴桥（亮脆），大=靠近琴颈（柔和）。
    </div>
    <div class="log" id="log"></div>
  </div>

  <div id="result" class="hidden">
    <h2>识别文本</h2>
    <div class="panel"><div class="transcript" id="text"></div></div>

    <h2>音频诊断 <span style="text-transform:none;font-weight:400">（用于分离硬件问题和云端问题）</span></h2>
    <div class="panel">
      <dl class="kv" id="audioKv"></dl>
      <div id="audioHints" style="margin-top:10px"></div>
    </div>

    <h2>音乐参数</h2>
    <div class="panel">
      <dl class="kv" id="musicKv"></dl>
      <div class="chords" id="chords"></div>
    </div>

    <h2>旋律钢琴卷帘 <span style="text-transform:none;font-weight:400">（横轴=时间格，纵轴=音高；灰色=伴奏，蓝色=主旋律）</span></h2>
    <div class="panel"><canvas id="roll" width="1860" height="300"></canvas></div>

    <h2>原始响应</h2>
    <div class="panel">
      <details><summary>展开完整 JSON</summary><pre id="raw"></pre></details>
    </div>
  </div>
</div>

<script>
(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };
  var mediaRecorder = null, chunks = [], recording = false;
  var lastMusic = null, lastLabels = null, playbackNodes = [];
  var player = null; // 当前播放会话；stopPlayback 靠它清掉排程定时器

  function log(msg) {
    var el = $('log');
    el.textContent += msg + '\\n';
    el.scrollTop = el.scrollHeight;
  }

  function setStatus(msg) { $('status').textContent = msg; }

  // ---------------------------------------------------------------------
  // 探活：顺手把当前 STAGE 显示出来
  // ---------------------------------------------------------------------
  fetch('/api/v1/health').then(function (r) { return r.json(); }).then(function (h) {
    var b = $('stage');
    b.textContent = 'STAGE=' + h.stage + (h.auth_required ? ' · 需令牌' : '');
    b.className = 'badge ' + (h.stage === 'full' ? 'ok' : 'warn');
    log('后端在线 stage=' + h.stage + ' auth=' + h.auth_required + ' ai=' + h.ai_binding);
  }).catch(function (e) { log('探活失败: ' + e); });

  // ---------------------------------------------------------------------
  // 录音
  // ---------------------------------------------------------------------
  $('btnRec').onclick = function () {
    if (recording) { stopRecording(); return; }
    startRecording();
  };

  function startRecording() {
    navigator.mediaDevices.getUserMedia({
      audio: {
        channelCount: 1,
        echoCancellation: false,   // 关掉浏览器音效，否则测的不是真实麦克风
        noiseSuppression: false,
        autoGainControl: false
      }
    }).then(function (stream) {
      chunks = [];
      mediaRecorder = new MediaRecorder(stream);
      mediaRecorder.ondataavailable = function (e) { if (e.data.size) chunks.push(e.data); };
      mediaRecorder.onstop = function () {
        stream.getTracks().forEach(function (t) { t.stop(); });
        var blob = new Blob(chunks, { type: chunks[0] ? chunks[0].type : 'audio/webm' });
        handleBlob(blob);
      };
      mediaRecorder.start();
      recording = true;
      $('btnRec').textContent = '\\u25A0 停止录音';
      $('btnRec').className = 'rec';
      setStatus('录音中… 说一句话，然后点停止');
      log('开始录音');

      // 最长 12 秒自动停，避免录出超长文件
      setTimeout(function () { if (recording) { log('达到 12 秒上限，自动停止'); stopRecording(); } }, 12000);
    }).catch(function (e) {
      log('拿不到麦克风权限: ' + e.message + '（需要 https 或 localhost）');
      setStatus('麦克风不可用');
    });
  }

  function stopRecording() {
    if (mediaRecorder && mediaRecorder.state !== 'inactive') mediaRecorder.stop();
    recording = false;
    $('btnRec').textContent = '\\u25CF 开始录音';
    $('btnRec').className = '';
    setStatus('处理中…');
  }

  // ---------------------------------------------------------------------
  // 压缩音频 -> 16kHz 单声道 WAV
  // ---------------------------------------------------------------------
  function handleBlob(blob) {
    blob.arrayBuffer().then(function (ab) {
      var AC = window.AudioContext || window.webkitAudioContext;
      var ctx = new AC();
      return ctx.decodeAudioData(ab).then(function (audioBuf) {
        ctx.close();
        // 重采样到 16kHz 单声道
        var targetRate = 16000;
        var frames = Math.max(1, Math.ceil(audioBuf.duration * targetRate));
        var OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
        var off = new OAC(1, frames, targetRate);
        var src = off.createBufferSource();
        src.buffer = audioBuf;
        src.connect(off.destination);
        src.start();
        return off.startRendering();
      });
    }).then(function (rendered) {
      var wav = encodeWav(rendered.getChannelData(0), 16000);
      log('已编码 WAV: ' + wav.byteLength + ' 字节, ' + rendered.duration.toFixed(2) + ' 秒');
      upload(wav);
    }).catch(function (e) {
      log('音频处理失败: ' + e.message);
      setStatus('音频处理失败');
    });
  }

  function encodeWav(samples, sampleRate) {
    var n = samples.length;
    var buf = new ArrayBuffer(44 + n * 2);
    var dv = new DataView(buf);
    function w(off, s) { for (var i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); }
    w(0, 'RIFF');  dv.setUint32(4, 36 + n * 2, true);
    w(8, 'WAVE');
    w(12, 'fmt '); dv.setUint32(16, 16, true);
    dv.setUint16(20, 1, true);                    // PCM
    dv.setUint16(22, 1, true);                    // 单声道
    dv.setUint32(24, sampleRate, true);
    dv.setUint32(28, sampleRate * 2, true);       // byteRate
    dv.setUint16(32, 2, true);                    // blockAlign
    dv.setUint16(34, 16, true);                   // 位深
    w(36, 'data'); dv.setUint32(40, n * 2, true);
    var o = 44;
    for (var i = 0; i < n; i++) {
      var s = Math.max(-1, Math.min(1, samples[i]));
      dv.setInt16(o, s < 0 ? s * 0x8000 : s * 0x7fff, true);
      o += 2;
    }
    return buf;
  }

  // ---------------------------------------------------------------------
  // 上传
  // ---------------------------------------------------------------------
  function upload(wav) {
    var headers = { 'Content-Type': 'audio/wav' };
    var tok = $('token').value.trim();
    if (tok) headers['Authorization'] = 'Bearer ' + tok;

    var t0 = performance.now();
    setStatus('上传并等待后端…');

    fetch('/api/v1/intent', { method: 'POST', headers: headers, body: wav })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, body: j }; }); })
      .then(function (res) {
        var ms = Math.round(performance.now() - t0);
        log('HTTP ' + res.status + ' 往返 ' + ms + 'ms');
        if (!res.body.ok) {
          log('后端返回错误: ' + (res.body.error || 'unknown'));
          setStatus('失败: ' + (res.body.error || 'unknown'));
        } else {
          setStatus('完成 · ' + ms + 'ms');
        }
        render(res.body);
      })
      .catch(function (e) {
        log('请求失败: ' + e.message);
        setStatus('请求失败');
      });
  }

  // ---------------------------------------------------------------------
  // 渲染结果
  // ---------------------------------------------------------------------
  function render(body) {
    $('result').classList.remove('hidden');
    $('raw').textContent = JSON.stringify(body, null, 2);
    $('text').textContent = body.text || '(无)';

    // 音频诊断
    var a = body.audio || {};
    var akv = [
      ['字节数', a.bytes],
      ['时长', a.durationSec !== undefined ? a.durationSec + ' 秒' : '-'],
      ['采样率', a.sampleRate],
      ['声道', a.channels],
      ['位深', a.bitsPerSample],
      ['RMS 音量', a.rms],
      ['峰值', a.peak],
      ['直流偏置', a.dc]
    ];
    $('audioKv').innerHTML = akv.map(function (p) {
      return '<dt>' + p[0] + '</dt><dd>' + (p[1] === undefined ? '-' : p[1]) + '</dd>';
    }).join('');

    var hints = a.hints || [];
    $('audioHints').innerHTML = hints.map(function (h) {
      var ok = h.indexOf('正常') >= 0;
      return '<div class="hint' + (ok ? ' ok' : '') + '">' + esc(h) + '</div>';
    }).join('');

    // 音乐参数
    var m = body.music;
    if (!m) { $('btnPlay').disabled = true; return; }
    lastMusic = m;
    var labels = body.labels || {};
    lastLabels = labels; // ★ 必须在 labels 赋值之后再存，否则 var 提升会拿到 undefined
    var mkv = [
      [
        '意图来源',
        body.intent_source === 'llm'
          ? '\u2705 LLM\uff08\u6b63\u5e38\uff0c\u97f3\u4e50\u4f1a\u8ddf\u7740\u4f60\u8bf4\u7684\u8bdd\u53d8\uff09'
          : '<span style="color:var(--err)">\u26a0\ufe0f FALLBACK\uff08LLM \u5931\u8d25\uff0c\u7528\u7684\u662f\u56fa\u5b9a\u9ed8\u8ba4\u7f16\u6392 \u2192 \u6240\u4ee5\u6c38\u8fdc\u4e00\u6837\uff09</span>'
      ],
      ['BPM', m.bpm],
      ['调式', (labels.key || m.key_index) + ' ' + (labels.scale || m.scale_index)],
      ['小节数', m.bars + ' 小节 / ' + m.beats_per_bar + ' 拍'],
      ['时间格', m.step_ms + ' ms（每拍 ' + m.steps_per_beat + ' 格）'],
      ['音色', (labels.instrument || m.instrument_index) + '（index ' + m.instrument_index + '）'],
      ['衰减', m.decay_seconds + ' 秒到 -60dB'],
      ['激励亮度', m.brightness],
      ['环路阻尼', m.damping !== undefined ? m.damping : '—'],
      ['拨弦位置', m.pick_position !== undefined ? m.pick_position : '—'],
      ['鼓型', (m.drum_pattern || 'none') + '（每小节 16 格 / ' + (m.drum_step_ms || '—') + 'ms）'],
      ['曲式', labels.form || '—'],
      ['伴奏音型', '[' + m.arp_pattern.join(', ') + ']'],
      ['和弦音高', '[' + m.chord_notes.join(', ') + ']'],
      ['旋律音高', '[' + m.melody_notes.join(', ') + ']（-1 = 休止）']
    ];
    $('musicKv').innerHTML = mkv.map(function (p) {
      return '<dt>' + p[0] + '</dt><dd>' + p[1] + '</dd>';
    }).join('');

    // 如果是兜底，把 LLM 的原始报错也显示出来，省得去翻 JSON
    if (body.intent_source !== 'llm' && body.intent_error) {
      $('musicKv').innerHTML +=
        '<dt>LLM 报错</dt><dd style="color:var(--err);word-break:break-all">' +
        esc(body.intent_error) +
        '</dd>';
    }

    $('chords').innerHTML = (labels.chords || []).map(function (c) {
      return '<span class="chip">' + esc(c) + '</span>';
    }).join('');

    drawRoll(m);
    $('btnPlay').disabled = false;
  }

  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }

  // ---------------------------------------------------------------------
  // 钢琴卷帘
  // ---------------------------------------------------------------------
  function drawRoll(m) {
    var cv = $('roll'), g = cv.getContext('2d');
    var W = cv.width, H = cv.height;
    g.clearRect(0, 0, W, H);

    var stepsPerBar = m.beats_per_bar * m.steps_per_beat;
    var total = Math.max(m.melody_notes.length, m.bars * stepsPerBar);
    if (!total) return;

    // 收集所有出现的音高，确定纵轴范围
    var lo = 127, hi = 0;
    m.chord_notes.concat(m.melody_notes).forEach(function (n) {
      if (n >= 0) { if (n < lo) lo = n; if (n > hi) hi = n; }
    });
    if (hi < lo) { lo = 48; hi = 84; }
    var pad = 2;
    lo -= pad; hi += pad;
    var span = hi - lo + 1;

    var x0 = 44, y0 = 12;
    var plotW = W - x0 - 16, plotH = H - y0 - 26;
    var stepW = plotW / total;
    var rowH = plotH / span;

    // 网格线
    g.strokeStyle = '#1c2230';
    g.lineWidth = 1;
    for (var b = 0; b <= m.bars; b++) {
      var x = x0 + b * stepsPerBar * stepW;
      g.beginPath(); g.moveTo(x, y0); g.lineTo(x, y0 + plotH); g.stroke();
    }
    // 每八度一条横线
    for (var n = Math.ceil(lo / 12) * 12; n <= hi; n += 12) {
      var y = y0 + (hi - n) * rowH;
      g.strokeStyle = '#232b3a';
      g.beginPath(); g.moveTo(x0, y); g.lineTo(x0 + plotW, y); g.stroke();
      g.fillStyle = '#5a6478';
      g.font = '11px ui-monospace, monospace';
      g.fillText('C' + (Math.floor(n / 12) - 1), 6, y + 4);
    }

    function midiToY(midi) { return y0 + (hi - midi) * rowH; }

    // 伴奏和弦：铺一层底色
    g.fillStyle = 'rgba(150,160,180,.28)';
    var off = 0;
    for (var bar = 0; bar < m.chord_note_counts.length; bar++) {
      var cnt = m.chord_note_counts[bar];
      for (var k = 0; k < cnt; k++) {
        var mn = m.chord_notes[off + k];
        if (mn < 0) continue;
        g.fillRect(x0 + bar * stepsPerBar * stepW, midiToY(mn), stepsPerBar * stepW, Math.max(2, rowH - 1));
      }
      off += cnt;
    }

    // 主旋律：一个音画一个小块
    var hold = m.melody_hold || [];
    for (var i = 0; i < m.melody_notes.length; i++) {
      var note = m.melody_notes[i];
      if (note < 0) continue;
      var w = stepW * (hold[i] || 1);
      g.fillStyle = '#4f8cff';
      g.fillRect(x0 + i * stepW, midiToY(note), Math.max(2, w - 1), Math.max(3, rowH - 1));
    }

    // 小节号
    g.fillStyle = '#5a6478';
    g.font = '11px ui-monospace, monospace';
    for (var bb = 0; bb < m.bars; bb++) {
      g.fillText(String(bb + 1), x0 + bb * stepsPerBar * stepW + 3, H - 8);
    }
  }

  // ---------------------------------------------------------------------
  // Karplus-Strong 合成
  //
  // 这段算法和 ESP32 端要实现的是同一套，参数全部来自后端返回的 music 字段。
  // 在这里调好听的参数，可以直接搬到 C 代码里（C 版本见 docs/API.md）。
  //
  // ★ 核心改进一：分数延迟（修音准）
  //   延迟线长度 N = 采样率 / 频率 必须是整数。直接 round() 取整会让每个音
  //   实际发出的频率偏离目标，而且不同音高偏的程度不一样 ——
  //   实测 C5 偏 -23.7 音分、G5 偏 +35 音分，一个和弦内部的走音跨度能到 13 音分，
  //   人耳完全听得出来（5-10 音分就可察觉）。
  //
  //   修法：N 取 floor，剩下的小数部分 frac 用一阶全通滤波器补足。
  //   全通在低频处的相位延迟 ≈ frac 个采样点。修完误差 < 0.5 音分。
  //
  // ★ 核心改进二：brightness 语义修正
  //   一阶低通 prev = a*prev + (1-a)*white 里，a 越大越"暗"。
  //   原代码直接写了 a = brightness，语义正好反了 ——
  //   于是"班卓"（brightness 0.85）反而成了最闷的音色。现在取 a = 1 - brightness。
  // ---------------------------------------------------------------------
  // ---------------------------------------------------------------------
  // 混响脉冲响应
  //
  // ★ 这里修的是一个我自己造出来的问题。
  //
  //   第一版"混响"用的是 4 条 30~80ms 的反馈延迟线。但那在声学上【不是混响，
  //   是梳状滤波器】—— 它会在 1/延迟 的整数倍频率上产生尖锐共振，
  //   听感就是一股金属味的嗡鸣，也就是"琴弦一直在震"。
  //
  //   正确做法：合成一段指数衰减的噪声当脉冲响应，交给 ConvolverNode 做真正的
  //   卷积混响。它的频响是平滑的，不会有梳状共振。
  // ---------------------------------------------------------------------
  function makeReverbIR(ctx, seconds, shape) {
    var len = Math.max(1, Math.round(seconds * ctx.sampleRate));
    var buf = ctx.createBuffer(2, len, ctx.sampleRate);
    for (var ch = 0; ch < 2; ch++) {
      var d = buf.getChannelData(ch);
      var lp = 0;
      for (var i = 0; i < len; i++) {
        var t = i / len;
        var noise = Math.random() * 2 - 1;
        lp = lp * 0.72 + noise * 0.28; // 高频先衰减 -> 更自然的房间感
        d[i] = lp * Math.pow(1 - t, shape);
      }
    }
    return buf;
  }

  // ---------------------------------------------------------------------
  // 鼓点合成
  //
  // ★ 不用任何采样 —— 三个基本音色都是几行 DSP，ESP32 上能原样照搬：
  //     kick  底鼓：正弦波 + 音高在 60ms 内从 120Hz 滑到 45Hz（"砰"的关键）
  //     snare 军鼓：高通噪声（沙沙）+ 190Hz 正弦（鼓皮，没有它就只有沙没有"啪"）
  //     hat   踩镲：极短的高通噪声
  // ---------------------------------------------------------------------
  // 统一追踪正在发声的节点。
  // 循环播放会不断产生新节点，不清理的话数组会无限增长；
  // 靠 onended 自动摘除，stopPlayback 时再统一 stop。
  function trackNode(node) {
    playbackNodes.push(node);
    node.onended = function () {
      var i = playbackNodes.indexOf(node);
      if (i >= 0) playbackNodes.splice(i, 1);
    };
  }

  function playKick(ctx, dest, when, gain) {
    var osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(120, when);
    osc.frequency.exponentialRampToValueAtTime(45, when + 0.06);
    var g = ctx.createGain();
    g.gain.setValueAtTime(gain, when);
    g.gain.exponentialRampToValueAtTime(0.0008, when + 0.3);
    osc.connect(g);
    g.connect(dest);
    osc.start(when);
    osc.stop(when + 0.32);
    trackNode(osc);
  }

  function noiseBuffer(ctx, seconds, shape) {
    var len = Math.max(1, Math.round(seconds * ctx.sampleRate));
    var buf = ctx.createBuffer(1, len, ctx.sampleRate);
    var d = buf.getChannelData(0);
    for (var i = 0; i < len; i++) {
      d[i] = (Math.random() * 2 - 1) * Math.pow(1 - i / len, shape);
    }
    return buf;
  }

  function playSnare(ctx, dest, when, gain) {
    var src = ctx.createBufferSource();
    src.buffer = noiseBuffer(ctx, 0.19, 2.4);
    var hp = ctx.createBiquadFilter();
    hp.type = 'highpass';
    hp.frequency.value = 1300;
    var g = ctx.createGain();
    g.gain.value = gain;
    src.connect(hp);
    hp.connect(g);
    g.connect(dest);
    src.start(when);
    trackNode(src);

    // 鼓皮音：纯噪声听起来只有"沙沙"，加个 190Hz 才有"啪"
    var osc = ctx.createOscillator();
    osc.type = 'triangle';
    osc.frequency.value = 190;
    var og = ctx.createGain();
    og.gain.setValueAtTime(gain * 0.55, when);
    og.gain.exponentialRampToValueAtTime(0.0008, when + 0.13);
    osc.connect(og);
    og.connect(dest);
    osc.start(when);
    osc.stop(when + 0.15);
    trackNode(osc);
  }

  function playHat(ctx, dest, when, gain) {
    var src = ctx.createBufferSource();
    src.buffer = noiseBuffer(ctx, 0.055, 3.2);
    var hp = ctx.createBiquadFilter();
    hp.type = 'highpass';
    hp.frequency.value = 7200;
    var g = ctx.createGain();
    g.gain.value = gain;
    src.connect(hp);
    hp.connect(g);
    g.connect(dest);
    src.start(when);
    trackNode(src);
  }

  function sliderVal(id) {
    var el = $(id);
    return el ? parseFloat(el.value) : 0;
  }

  // 滑块数值实时回显
  ['sRev', 'sDecay', 'sBright', 'sOverlap', 'sDrum', 'sDamp', 'sPick', 'sLoop'].forEach(function (id) {
    var el = $(id);
    if (!el) return;
    el.oninput = function () {
      var label = $('v' + id.slice(1));
      if (!label) return;
      label.textContent =
        id === 'sOverlap' || id === 'sLoop'
          ? (parseFloat(el.value) ? '开' : '关')
          : parseFloat(el.value).toFixed(2);
    };
  });

  function ksRender(midi, seconds, decaySeconds, brightness, sr, damping, pickPos) {
    var freq = 440 * Math.pow(2, (midi - 69) / 12);
    var w = (2 * Math.PI * freq) / sr;
    var dmp = Math.max(0, Math.min(0.6, damping || 0));

    // ---- 扩展 KS 第 0 步（最关键，最容易漏）：先算清环路滤波器的相位延迟 ----
    //
    // 环路里每加一个滤波器都会带来额外的相位延迟。如果不从延迟线长度里扣掉，
    // 总延迟就变长、音高会偏低 —— 实测能偏到 -45 音分，比不做分数延迟还糟。
    //
    // 一阶低通 (1-d)/(1 - d z^-1) 在频率 w 处的相位延迟 = atan2(d·sin w, 1 - d·cos w) / w
    var lpPhase = dmp > 0 ? Math.atan2(dmp * Math.sin(w), 1 - dmp * Math.cos(w)) / w : 0;

    var totalDelay = sr / freq - lpPhase;
    if (totalDelay < 2) totalDelay = 2;

    var N = Math.floor(totalDelay);
    var frac = totalDelay - N;
    // 一阶全通系数：低频处的相位延迟 ≈ frac 个采样点
    var eta = (1 - frac) / (1 + frac);

    // ---- 激励：一阶低通白噪声填满延迟线 ----
    // a = 1 - brightness，这样 brightness 越大 = 保留高频越多 = 拨弦越"脆"
    var a = 1 - Math.max(0, Math.min(0.98, brightness));
    var prev = 0;
    var buf = new Float32Array(N);
    for (var i = 0; i < N; i++) {
      var white = Math.random() * 2 - 1;
      prev = a * prev + (1 - a) * white;
      buf[i] = prev;
    }

    // ---- 扩展 KS 第 1 步：拨弦位置梳状滤波 ----
    // 在离琴桥 pickPos 比例的位置拨弦，会在对应谐波上产生陷波。
    // 这是"听起来像被拨出来的"而不是"像合成器"的主要来源之一。
    if (pickPos > 0.02 && pickPos < 0.5) {
      var M = Math.max(1, Math.round(N * pickPos));
      var tmp = buf.slice();
      for (var k = 0; k < N; k++) {
        buf[k] = tmp[k] - tmp[(k - M + N) % N];
      }
    }

    // 每样点反馈系数：与频率无关（推导见 worker/src/music.js）
    var g = Math.pow(0.001, 1 / (sr * decaySeconds));

    var len = Math.max(1, Math.round(seconds * sr));
    var out = new Float32Array(len);

    var idx = 0;
    var apX1 = 0;
    var apY1 = 0; // 全通状态
    var lpZ = 0; // 环路低通状态

    for (var j = 0; j < len; j++) {
      var raw = buf[idx];

      // 1) 全通分数延迟 —— 音准
      var ap = eta * raw + apX1 - eta * apY1;
      apX1 = raw;
      apY1 = ap;

      // 2) 扩展 KS 第 2 步：环路阻尼低通
      //    ★ 和普通 KS 的"两点平均"关键区别：这里的 dmp 是独立可调的，
      //      于是「整体延音很长」和「高频很快消失」可以同时成立。
      //      普通 KS 里这两件事被延迟线长度绑死了，调不了。
      lpZ = (1 - dmp) * ap + dmp * lpZ;

      // 3) 反馈衰减
      buf[idx] = g * lpZ;

      idx = (idx + 1) % N;
      out[j] = ap;
    }
    return out;
  }

  $('btnPlay').onclick = function () {
    var m = lastMusic;
    if (!m) return;
    stopPlayback();

    var AC = window.AudioContext || window.webkitAudioContext;
    var ctx = new AC();
    var sr = ctx.sampleRate;
    var stepSec = m.step_ms / 1000;
    var t0 = ctx.currentTime + 0.15;

    // --- 信号链：干声 + 混响 ---
    var dryGain = ctx.createGain();
    dryGain.gain.value = 1.0;
    var wetGain = ctx.createGain();
    wetGain.gain.value = sliderVal('sRev');
    var master = ctx.createGain();
    master.gain.value = 0.36;
    dryGain.connect(master);
    wetGain.connect(master);
    master.connect(ctx.destination);

    // ★ 用卷积混响取代原来的梳状滤波器
    var conv = ctx.createConvolver();
    conv.buffer = makeReverbIR(ctx, 1.6, 2.6);
    conv.connect(wetGain);
    var revIn = ctx.createGain();
    revIn.connect(conv);

    // ---------------------------------------------------------------------
    // 参数：每次排程时才读，而不是按播放键时读一次
    //
    // ★ 这是「播放中调参能实时生效」的关键。原来的写法把参数一次性固化，
    //   排好的音已经带上旧参数了，拖滑块当然没反应。
    // ---------------------------------------------------------------------
    function readParams() {
      return {
        decay: sliderVal('sDecay') || m.decay_seconds || 2.0,
        bright: sliderVal('sBright') || m.brightness || 0.6,
        damp: sliderVal('sDamp'),
        pick: sliderVal('sPick'),
        drum: sliderVal('sDrum'),
        rev: sliderVal('sRev'),
        noOverlap: !sliderVal('sOverlap'),
      };
    }

    // 渲染缓存：按「音高 + 音色参数」缓存，参数没变就不会重算
    var cache = {};
    function bufferFor(midi, p) {
      var k = midi + ':' + p.decay + ':' + p.bright + ':' + p.damp + ':' + p.pick;
      if (cache[k]) return cache[k];
      if (Object.keys(cache).length > 500) cache = {}; // 防止无限增长
      var ring = Math.min(Math.max(p.decay, 0.5), 8);
      var data = ksRender(midi, ring, p.decay, p.bright, sr, p.damp, p.pick);
      var ab = ctx.createBuffer(1, data.length, sr);
      ab.getChannelData(0).set(data);
      cache[k] = ab;
      return ab;
    }

    // 正在发声的声部，按音高索引 —— 用于「同音重触发掐断」
    var activeVoices = {};

    function schedule(midi, when, gain, p) {
      if (midi < 0 || midi > 127) return;

      // 同一个音高再次被拨响时，真实弦上"上一次的振动"就结束了。
      // 不掐断的话，两个同频波会互相干涉，产生打拍般的嗡鸣。
      //
      // ★ 必须用 when（新音触发时刻）而不是 ctx.currentTime：
      //   音是提前排在未来的，用 currentTime 会把还没响的音提前 stop 掉。
      if (p.noOverlap && activeVoices[midi]) {
        var old = activeVoices[midi];
        try {
          old.gain.gain.setValueAtTime(old.gain.gain.value, when);
          old.gain.gain.linearRampToValueAtTime(0, when + 0.012);
          old.src.stop(when + 0.03);
        } catch (e) {
          /* 已经停了 */
        }
      }

      var src = ctx.createBufferSource();
      src.buffer = bufferFor(midi, p);
      var g = ctx.createGain();
      g.gain.value = gain;
      src.connect(g);
      g.connect(dryGain);
      g.connect(revIn);
      src.start(when);
      trackNode(src);
      if (p.noOverlap) activeVoices[midi] = { src: src, gain: g };
    }

    var stepsPerBar = m.beats_per_bar * m.steps_per_beat;
    var barSec = stepsPerBar * stepSec;
    var totalBars = Math.max(1, m.bars);
    var tailSec = Math.min(Math.max(m.decay_seconds || 2, 1), 6);

    // 每个小节和弦音的起始下标
    var offsets = [];
    var acc = 0;
    for (var i = 0; i < m.chord_note_counts.length; i++) {
      offsets.push(acc);
      acc += m.chord_note_counts[i];
    }

    var bassNotes = m.bass_notes || [];
    var drumLevels = m.drum_level || [];

    // --- 鼓点总线 ---
    // 底鼓走纯干声（低频加混响会糊成一团），军鼓和踩镲各给一点湿润
    var kickBus = ctx.createGain();
    kickBus.gain.value = 1.0;
    kickBus.connect(dryGain);

    var snareBus = ctx.createGain();
    snareBus.gain.value = 1.0;
    snareBus.connect(dryGain);
    var snareWet = ctx.createGain();
    snareWet.gain.value = 0.3;
    snareBus.connect(snareWet);
    snareWet.connect(revIn);

    var hatBus = ctx.createGain();
    hatBus.gain.value = 1.0;
    hatBus.connect(dryGain);
    var hatWet = ctx.createGain();
    hatWet.gain.value = 0.15;
    hatBus.connect(hatWet);
    hatWet.connect(revIn);

    // --- 鼓点：独立网格，每小节 16 个 16 分音符 ---
    // 刻意不复用主网格（steps_per_beat）：旋律用 8 分音符就够，
    // 但踩镲经常需要 16 分，塞进同一张网格会不够用。
    var kick = m.drum_kick || [];
    var snare = m.drum_snare || [];
    var hat = m.drum_hat || [];
    var drumPerBar = 16;
    var drumStepSec = (m.drum_step_ms || Math.round(60000 / m.bpm / 4)) / 1000;

    function loopEnabled() {
      var el = $('sLoop');
      return el ? parseFloat(el.value) > 0 : true;
    }

    // ---------------------------------------------------------------------
    // 前瞻式排程（真正的音序器做法）
    //
    // ★ 为什么不能"一次性把整首排完"：
    //   那样所有音色参数在按下播放键的瞬间就被烘焙进已排好的音里了，
    //   播放中拖滑块完全没反应 —— 而"边听边调"正是调音最需要的能力。
    //
    //   现在每 100ms 醒一次，只把「未来 LOOKAHEAD 秒内」的小节排进时间轴。
    //   于是：拖滑块最多 1.5 秒就听到变化；循环开关也能实时切换。
    //   顺带还让循环播放变得很自然 —— 游标一直加，小节号取模就是循环。
    // ---------------------------------------------------------------------
    var LOOKAHEAD = 1.5;
    var nextBar = 0;

    function scheduleBar(n, p) {
      var localBar = n % totalBars;
      var barStart = t0 + n * barSec;
      var lvl = drumLevels[localBar] !== undefined ? drumLevels[localBar] / 9 : 1;

      for (var s = 0; s < stepsPerBar; s++) {
        var when = barStart + s * stepSec;
        var gi = localBar * stepsPerBar + s;
        var isDownbeat = s === 0;
        var isMidbeat = s === 2 * m.steps_per_beat;
        // 力度：小节第一拍最重、第三拍次之、其它最轻。
        // 固定力度听起来很机械，加上强弱立刻有"呼吸"。
        var vel = isDownbeat ? 1.0 : isMidbeat ? 0.78 : 0.6;

        if (m.strum) {
          // 扫弦：一次性拨响整个和弦，各弦错开几毫秒（模拟手指扫过琴弦）
          var spread = (m.strum_spread_ms || 22) / 1000;
          for (var k = 0; k < m.chord_note_counts[localBar]; k++) {
            schedule(m.chord_notes[offsets[localBar] + k], when + k * spread, 0.15 * vel, p);
          }
        } else {
          var toneIdx = m.arp_pattern[gi % m.arp_pattern.length] % m.chord_note_counts[localBar];
          schedule(m.chord_notes[offsets[localBar] + toneIdx], when, 0.24 * vel, p);
        }

        // 低音：每小节第一拍弹一次根音，让整段音乐"有底"
        if (isDownbeat && bassNotes[localBar] !== undefined) {
          schedule(bassNotes[localBar], when, 0.44, p);
        }

        // 主旋律
        if (gi < m.melody_notes.length) {
          schedule(m.melody_notes[gi], when, 0.38 * (isDownbeat ? 1.0 : 0.85), p);
        }
      }

      // 鼓点：强度 = 滑块 × 该小节的段落动态
      if (p.drum > 0 && lvl > 0 && (kick.length || snare.length || hat.length)) {
        var dl = p.drum * lvl;
        for (var d = 0; d < drumPerBar; d++) {
          var dw = barStart + d * drumStepSec;
          if (kick[d]) playKick(ctx, kickBus, dw, 0.85 * dl);
          if (snare[d]) playSnare(ctx, snareBus, dw, 0.42 * dl);
          if (hat[d]) playHat(ctx, hatBus, dw, 0.13 * dl);
        }
      }
    }

    var state = { timer: null };
    player = state;

    state.timer = setInterval(function () {
      var p = readParams();
      wetGain.gain.value = p.rev; // 混响量是纯增益，可以立刻改

      var looping = loopEnabled();
      var horizon = ctx.currentTime + LOOKAHEAD;

      while (t0 + nextBar * barSec < horizon) {
        if (!looping && nextBar >= totalBars) break;
        scheduleBar(nextBar, p);
        nextBar++;
      }

      // 不循环时，等最后一个小节播完 + 混响尾巴散掉，再停
      if (!looping && nextBar >= totalBars && ctx.currentTime > t0 + totalBars * barSec + tailSec) {
        setStatus('播放结束');
        stopPlayback();
      }
    }, 100);

    $('btnStop').disabled = false;
    setStatus('播放中…');
    log(
      '开始播放：' +
        totalBars +
        ' 小节 × ' +
        barSec.toFixed(2) +
        's ≈ ' +
        (totalBars * barSec).toFixed(1) +
        ' 秒，鼓型 ' +
        (m.drum_pattern || 'none') +
        '，曲式 ' +
        ((lastLabels && lastLabels.form) || '-')
    );
    log('★ 播放中拖滑块会实时生效（最多延迟约 1.5 秒）');
  };

  function stopPlayback() {
    // 必须先停掉排程定时器 —— 否则它会继续往时间轴上排新的音，
    // 表现就是"按了停止但还在响"。
    if (player) {
      clearInterval(player.timer);
      player = null;
    }
    playbackNodes.forEach(function (n) { try { n.stop(); } catch (e) {} });
    playbackNodes = [];
    $('btnStop').disabled = true;
  }
  $('btnStop').onclick = function () { stopPlayback(); setStatus('已停止'); };
})();
</script>
</body>
</html>`;
