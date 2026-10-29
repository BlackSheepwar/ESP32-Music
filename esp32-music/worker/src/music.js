/**
 * 确定性乐理展开层
 * ===========================================================================
 *
 * 这一层解决一个核心问题：**乐理放在哪一端？**
 *
 * 方案 A：让 LLM 直接吐 MIDI 音高数组
 *   -> 不可靠。LLM 经常在数值音乐理论上犯错（和声算错、音域越界、
 *      数组长度对不上），而且每次结果不稳定，你没法调试。
 *
 * 方案 B（本文件）：LLM 只负责「音乐意图」（符号层），
 *   数字层在这里用确定性代码展开。
 *   -> LLM 输出 "Cmaj7" 这种符号，出错一眼能看出来
 *   -> 展开是纯函数，同样输入永远同样输出，可以写单元测试
 *   -> ESP32 端拿到的永远是合法数组，设备上不需要任何乐理代码
 *
 * 这一层是「把复杂度留在有工具、有日志、能热更新的那一端」的典型应用。
 * ESP32 上烧一次固件要几十秒，改一行乐理逻辑要重新烧；在 Worker 上
 * 改完 `wrangler deploy` 十秒生效。
 *
 * ★ 所有下标（chord_types / instrument_index / scale_index）都会直接下发
 *   给 ESP32 并写进 C 语言的查表数组里。所以：
 *   【这些表的已有顺序只能追加，绝对不能改动】
 */

// ---------------------------------------------------------------------------
// 音名
// ---------------------------------------------------------------------------

export const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

const NOTE_PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

// ---------------------------------------------------------------------------
// 和弦类型表
// index 直接下发给 ESP32，顺序不可变，只能往后追加
// ---------------------------------------------------------------------------

export const CHORD_TYPES = [
  { key: 'maj', intervals: [0, 4, 7] }, // 0  大三和弦
  { key: 'min', intervals: [0, 3, 7] }, // 1  小三和弦
  { key: 'maj7', intervals: [0, 4, 7, 11] }, // 2  大七
  { key: 'min7', intervals: [0, 3, 7, 10] }, // 3  小七
  { key: 'dom7', intervals: [0, 4, 7, 10] }, // 4  属七
  { key: 'dim', intervals: [0, 3, 6] }, // 5  减三
  { key: 'aug', intervals: [0, 4, 8] }, // 6  增三
  { key: 'sus4', intervals: [0, 5, 7] }, // 7  挂四
  { key: 'sus2', intervals: [0, 2, 7] }, // 8  挂二
  { key: 'six', intervals: [0, 4, 7, 9] }, // 9  大六
  { key: 'min6', intervals: [0, 3, 7, 9] }, // 10 小六
  { key: 'm7b5', intervals: [0, 3, 6, 10] }, // 11 半减七
  { key: 'add9', intervals: [0, 4, 7, 14] }, // 12 加九
];

/** 后缀 -> 和弦类型下标。同一个类型的所有常见写法都要覆盖到。 */
const SUFFIX_TO_TYPE = {
  '': 0,
  maj: 0,
  M: 0,
  m: 1,
  min: 1,
  '-': 1,
  maj7: 2,
  M7: 2,
  'Δ': 2,
  'Δ7': 2,
  m7: 3,
  min7: 3,
  '-7': 3,
  '7': 4,
  dom7: 4,
  dim: 5,
  o: 5,
  aug: 6,
  '+': 6,
  sus4: 7,
  sus: 7,
  sus2: 8,
  '6': 9,
  maj6: 9,
  m6: 10,
  min6: 10,
  m7b5: 11,
  'ø': 11,
  'ø7': 11,
  add9: 12,
};

// 按长度倒序匹配，保证 "m7b5" 不会被 "m7" 抢先命中
const SUFFIX_KEYS = Object.keys(SUFFIX_TO_TYPE)
  .filter((k) => k.length > 0)
  .sort((a, b) => b.length - a.length);

/**
 * 解析和弦符号 -> { root, typeIndex, intervals }
 * 支持: C / Cm / Cmaj7 / F#m7 / Bb / G7 / Dm7b5 / Csus4 / Cadd9 / C/E(转位)
 * 解析失败返回 null，调用方负责兜底。
 */
export function parseChord(symbol) {
  const raw = String(symbol ?? '').trim();
  const m = /^([A-Ga-g])([#b♯♭]?)(.*)$/.exec(raw);
  if (!m) return null;

  let root = NOTE_PC[m[1].toUpperCase()];
  const acc = m[2];
  if (acc === '#' || acc === '♯') root = (root + 1) % 12;
  else if (acc === 'b' || acc === '♭') root = (root + 11) % 12;

  // 丢掉转位斜杠记号：C/E 按 C 处理
  let suffix = m[3].split('/')[0].replace(/\s+/g, '');
  // 归一化别名
  suffix = suffix.replace(/^min/i, 'm').replace(/^maj/i, 'maj').replace(/^dom/i, '');

  let typeIndex = 0;
  if (suffix.length > 0) {
    let matched = null;
    for (const key of SUFFIX_KEYS) {
      if (suffix.startsWith(key)) {
        matched = key;
        break;
      }
    }

    if (matched) {
      // 命中了已知后缀就直接用；多余部分（如 "maj9" 的 "9"）忽略，
      // 降级为对应的基础和弦，而不是整个丢掉
      typeIndex = SUFFIX_TO_TYPE[matched];

      // 延伸音（9/11/13）在乐理上隐含一个七度音，所以要把三和弦升级成对应的七和弦。
      // "Cmaj9" 应该得到 maj7，而不是丢成普通的 C。
      const rest = suffix.slice(matched.length);
      if (/^(9|11|13)/.test(rest)) {
        if (typeIndex === 0) typeIndex = 2; // maj -> maj7
        else if (typeIndex === 1) typeIndex = 3; // min -> min7
      }
    } else if (/^(9|11|13)/.test(suffix)) {
      // 光秃秃的 "C9" 是属七和弦（C E G Bb D），不是大三和弦
      typeIndex = 4;
    } else if (/^[0-9#b+()]*$/.test(suffix)) {
      // 其他未建模的延伸记号，当大三和弦处理
      typeIndex = 0;
    } else {
      // 后缀里有无法识别的字母 —— 这压根不是和弦符号。
      // 必须返回 null，否则 "garbage" 会被静默解析成 G 大三和弦。
      return null;
    }
  }

  return { root, typeIndex, intervals: CHORD_TYPES[typeIndex].intervals, symbol: raw };
}

// ---------------------------------------------------------------------------
// 音阶表（同样：顺序不可变，只能追加）
// ---------------------------------------------------------------------------

export const SCALES = [
  { key: 'major', steps: [0, 2, 4, 5, 7, 9, 11] }, // 0 自然大调
  { key: 'minor', steps: [0, 2, 3, 5, 7, 8, 10] }, // 1 自然小调
  { key: 'harmonic_minor', steps: [0, 2, 3, 5, 7, 8, 11] }, // 2 和声小调
  { key: 'dorian', steps: [0, 2, 3, 5, 7, 9, 10] }, // 3 多利亚
  { key: 'mixolydian', steps: [0, 2, 4, 5, 7, 9, 10] }, // 4 混合利底亚
  { key: 'pentatonic_major', steps: [0, 2, 4, 7, 9] }, // 5 大调五声
  { key: 'pentatonic_minor', steps: [0, 3, 5, 7, 10] }, // 6 小调五声
  { key: 'blues', steps: [0, 3, 5, 6, 7, 10] }, // 7 布鲁斯
];

const SCALE_ALIASES = {
  major: 0, ionian: 0, maj: 0, 大调: 0,
  minor: 1, aeolian: 1, min: 1, 小调: 1,
  harmonic_minor: 2, 和声小调: 2,
  dorian: 3, 多利亚: 3,
  mixolydian: 4, 混合利底亚: 4,
  pentatonic_major: 5, major_pentatonic: 5, 五声大调: 5,
  pentatonic_minor: 6, minor_pentatonic: 6, 五声小调: 6,
  blues: 7, 布鲁斯: 7,
};

export function findScaleIndex(name) {
  const k = String(name ?? '').trim().toLowerCase().replace(/[\s-]+/g, '_');
  if (k in SCALE_ALIASES) return SCALE_ALIASES[k];
  const byKey = SCALES.findIndex((s) => s.key === k);
  return byKey >= 0 ? byKey : 0;
}

// ---------------------------------------------------------------------------
// 音色表
//
// ★ 这里对 Karplus-Strong 的衰减参数做了一个刻意的重新参数化，值得解释：
//
//   常见写法是把「延迟线反馈系数」直接下发，例如 damping = 0.996。
//   但这个写法有个物理上反直觉的后果：KS 里信号每绕延迟线一圈才衰减一次，
//   而延迟线长度 N = 采样率 / 频率。于是「每周期保留比例」= damping^N，
//   频率越低 N 越大 -> 衰减越快。
//   => 低音比高音死得更快，正好和真实弦乐器相反。
//
//   所以这里下发的是 decay_seconds：**衰减到 -60dB 需要的秒数**。
//   设备端这样换算系数（推导见下）：
//
//       N        = 采样率 / 频率              // 延迟线长度（每周期样点数）
//       P        = 频率 * decay_seconds       // 总共经历多少个周期
//       每次绕圈的保留比例 = 0.001^(1/P)
//       每样点的系数 d     = (0.001^(1/P))^(1/N) = 0.001^(1/(P*N))
//                          = 0.001^(1/(采样率 * decay_seconds))   ← 频率被消掉了！
//
//   结果：d 与频率无关，每个音色只需要一个常数，在初始化时 powf 一次即可。
//   所有音高都会在同样的墙钟时间里衰减到 -60dB，听感才对。
//
// brightness —— 初始激励的低通强度 0..1。越大拨弦瞬间越"脆"
//
// ★ decay_seconds 的取值经验（踩过坑）：
//   这是「衰减到 -60dB 的秒数」，而 -60dB 已经几乎是听不见了，
//   所以它约等于「这个音能被听到多久」。
//
//   一开始我给了 4~8 秒，结果在持续演奏（每秒 2~3 个音）的场景下，
//   同时有十几个音在响，混成一片嗡鸣，听感是"琴弦一直在震停不下来"。
//
//   真实拨弦乐器在正常演奏时，一个音的有效可闻时长大约 1~2.5 秒
//   （低音弦长一点，高音短一点）。所以这里取 1~3 秒区间。
//
//   注意：如果做的是「单音旋律演示」而不是「伴奏」，可以调长一些。
// ---------------------------------------------------------------------------

// ★ 下面这组数值是在测试台上【听感调出来的】，不是理论推导的。
//
//   调音时用的那组滑块值（也是 demo-page 里滑块的默认值）：
//     混响 0.16 / 衰减 2.0s / 亮度 0.60 / 叠音 关 / 鼓点 0.80
//     阻尼 0.38 / 拨弦位 0.40 / 循环 开
//
//   其中「阻尼 0.38 + 拨弦位 0.40」就是尼龙弦吉他那一行。
//   其余音色是在这组基准上，按各自的性格平移得到的：
//     钢弦更亮更脆 -> 阻尼更低、拨弦位更靠琴桥
//     竖琴延音长、音色柔 -> 阻尼最低、拨弦位最靠琴颈
//     班卓短促明亮 -> 延音最短、阻尼低、拨弦位靠琴桥
//
//   以后要再调，建议在测试台上听，调好后把滑块值搬到这里 ——
//   这样 ESP32 端（没有滑块）拿到的也是同一个音色。

export const INSTRUMENTS = [
  // damping       —— 扩展 KS 的环路阻尼（0~0.6）
  //                  控制「高频消失得多快」，和 decay_seconds（整体延音）是两件独立的事。
  //                  真实弦就是这样：低音能嗡很久，但高音很快就只剩基频。
  //                  0.15 左右偏亮、0.38 左右偏"木"（实测好听）、0.55 已经很闷。
  // pick_position —— 拨弦位置（0~0.5，比例值）
  //                  在离琴桥这个比例的位置拨弦，会在对应谐波上产生陷波。
  //                  这是"听起来像被拨出来的"而不是"像合成器"的关键。
  //                  0.12 靠近琴桥（亮、脆）；0.28 中间；0.40 靠近琴颈（柔和、圆）
  { key: 'nylon_guitar', decay_seconds: 2.0, brightness: 0.60, damping: 0.38, pick_position: 0.40 }, // 0 ★基准
  { key: 'steel_guitar', decay_seconds: 1.8, brightness: 0.78, damping: 0.32, pick_position: 0.28 }, // 1
  { key: 'clean_electric', decay_seconds: 2.5, brightness: 0.45, damping: 0.46, pick_position: 0.34 }, // 2
  { key: 'harp', decay_seconds: 3.5, brightness: 0.35, damping: 0.24, pick_position: 0.50 }, // 3
  { key: 'banjo', decay_seconds: 1.0, brightness: 0.85, damping: 0.28, pick_position: 0.24 }, // 4
  { key: 'bass_pluck', decay_seconds: 2.1, brightness: 0.30, damping: 0.53, pick_position: 0.45 }, // 5
];

const INSTRUMENT_ALIASES = {
  nylon_guitar: 0, classical_guitar: 0, guitar: 0, acoustic: 0, 吉他: 0, 木吉他: 0, 尼龙: 0,
  steel_guitar: 1, folk_guitar: 1, 钢弦: 1, 民谣吉他: 1,
  clean_electric: 2, electric_guitar: 2, 电吉他: 2,
  harp: 3, 竖琴: 3,
  banjo: 4, 班卓: 4,
  bass_pluck: 5, bass: 5, 贝斯: 5,
};

export function findInstrumentIndex(name) {
  const k = String(name ?? '').trim().toLowerCase().replace(/[\s-]+/g, '_');
  if (k in INSTRUMENT_ALIASES) return INSTRUMENT_ALIASES[k];
  const byKey = INSTRUMENTS.findIndex((i) => i.key === k);
  return byKey >= 0 ? byKey : 0;
}

// ---------------------------------------------------------------------------
// 伴奏音型
// 数组里的数字 = 当前小节和弦的「第几个和弦音」（对和弦音个数取模）
// 例如 Cmaj7 = [C,E,G,B]，音型 [0,1,2,3] 展开就是 C E G B
// ---------------------------------------------------------------------------

export const ARP_PATTERNS = {
  arp_up: [0, 1, 2, 3],
  arp_updown: [0, 1, 2, 3, 2, 1],
  arp_wave: [0, 1, 2, 3, 2, 1, 2, 3],
  arp_16th: [0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3],
  fingerpick: [0, 2, 1, 3, 0, 2, 1, 3],
  travis: [0, 2, 1, 3, 0, 3, 1, 2],
  strum: [0, 0, 0, 0],
  bass_only: [0],
};

// ---------------------------------------------------------------------------
// 鼓点
//
// ★ 网格固定为 16 分音符，每小节 16 格 —— 刻意独立于 steps_per_beat。
//   旋律/伴奏用 8 分音符网格就够，但鼓点经常需要 16 分（尤其踩镲），
//   用同一张网格会不够用。
//
// ★ 不需要任何采样。三个基本音色都是几行 DSP 就能合成出来的，
//   所以 ESP32 端也能照搬（C 实现见 docs/API.md）：
//     kick  底鼓：正弦波 + 音高在 60ms 内从 120Hz 下滑到 45Hz
//     snare 军鼓：高通噪声 + 190Hz 的"鼓皮"正弦
//     hat   踩镲：极短的高通噪声（截止 7kHz 以上）
// ---------------------------------------------------------------------------

export const DRUM_PATTERNS = {
  // 最常规的流行/摇滚底鼓-军鼓-八分踩镲
  straight: {
    kick: [1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0],
    snare: [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0],
    hat: [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0],
  },
  // 加一点切分，更"流行"（华语流行、R&B、citypop 都用这类）
  pop: {
    kick: [1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0],
    snare: [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1],
    hat: [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1],
  },
  // 更密的踩镲 + 干净的底鼓军鼓
  rock: {
    kick: [1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0],
    snare: [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0],
    hat: [1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1],
  },
  // 四踩（four-on-the-floor）
  edm: {
    kick: [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0],
    snare: [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0],
    hat: [0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1],
  },
  // 稀疏：只在必要的地方点一下
  ballad: {
    kick: [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0],
    snare: [0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0],
    hat: [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0],
  },
  none: { kick: [], snare: [], hat: [] },
};

/** 风格 -> 鼓型。用 includes 匹配，所以 LLM 写 "hard rock" 也能命中 rock */
const STYLE_TO_DRUMS = [
  ['classical', 'none'],
  ['orchestral', 'none'],
  ['ambient', 'none'],
  ['ballad', 'ballad'],
  ['folk', 'ballad'],
  ['acoustic', 'ballad'],
  ['metal', 'rock'],
  ['punk', 'rock'],
  ['rock', 'rock'],
  ['edm', 'edm'],
  ['electronic', 'edm'],
  ['dance', 'edm'],
  ['house', 'edm'],
  ['techno', 'edm'],
  ['jazz', 'none'], // 爵士的摇摆律动用这套网格做不出来，宁可不加
  ['blues', 'ballad'],
];

function pickDrumPattern(styleKey, instrumentIndex) {
  // 竖琴（index 3）是独奏乐器，加鼓会毁掉它
  if (instrumentIndex === 3) return 'none';
  const k = String(styleKey ?? '').toLowerCase();
  for (const [needle, pattern] of STYLE_TO_DRUMS) {
    if (k.includes(needle)) return pattern;
  }
  return 'pop';
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

function clampInt(v, lo, hi, fallback) {
  const n = Math.round(Number(v));
  if (!Number.isFinite(n)) return fallback;
  return Math.min(hi, Math.max(lo, n));
}

/** 接受 0-11 / "C" / "F#" / "Bb" / "Db"，统一成 0-11 的音级 */
function pitchClass(v) {
  if (typeof v === 'number' && Number.isFinite(v)) {
    return ((Math.round(v) % 12) + 12) % 12;
  }
  const s = String(v ?? '').trim();
  const m = /^([A-Ga-g])([#b♯♭]?)$/.exec(s);
  if (!m) return 0;
  let pc = NOTE_PC[m[1].toUpperCase()];
  if (m[2] === '#' || m[2] === '♯') pc = (pc + 1) % 12;
  if (m[2] === 'b' || m[2] === '♭') pc = (pc + 11) % 12;
  return pc;
}

/** FNV-1a：把提示文本变成可复现的随机种子 */
function hashString(s) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

/** mulberry32：小巧的确定性伪随机数发生器 */
function mulberry32(a) {
  return function () {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function fitRange(midi, lo, hi) {
  let n = midi;
  while (n < lo) n += 12;
  while (n > hi) n -= 12;
  return n;
}

// ---------------------------------------------------------------------------
// 展开
// ---------------------------------------------------------------------------

/**
 * 兜底和弦走向。
 *
 * 大调：I - vi - ii - V   ->  Cmaj7 Am7 Dm7 G7
 * 小调：i - VI - III - VII ->  Am7 Fmaj7 Cmaj7 G7
 *
 * ★ 注意和弦性质必须跟着音阶级数走，这是最容易写错的地方：
 *   自然小调的 III 级是**大三和弦**（A 小调里 C-E-G），
 *   如果照搬大调模板写成 min7 就会得到 Cmin7，那不是调内和弦，一听就错。
 *   小调的调内七和弦依次是：min7 / m7b5 / maj7 / min7 / min7 / maj7 / dom7
 */
function defaultProgression(keyIndex, scaleIndex) {
  const isMajor = scaleIndex === 0 || scaleIndex === 5;
  const offsets = isMajor ? [0, 9, 2, 7] : [0, 8, 3, 10];
  const types = isMajor
    ? ['maj7', 'm7', 'm7', '7'] // I  - vi - ii - V
    : ['m7', 'maj7', 'maj7', '7']; // i - VI - III - VII
  return offsets.map((o, i) => PITCH_NAMES[(keyIndex + o) % 12] + types[i]);
}

/**
 * 把一个和弦展开成实际发声的 MIDI 音高，并做「声部连接」（voice leading）。
 *
 * ★ 原来每个和弦都用根位（base = 48 + root），结果是和弦进行时低音在
 *   45~60 之间来回乱跳（C→A→D→G 就是 48→57→50→55），听感很"笨"。
 *
 *   改进：枚举「八度位置 × 转位」，挑一个相对上一个和弦总移动量最小的声位。
 *   低音线会立刻平滑下来 —— 这是古典和声里最基础的 voice leading 思路，
 *   代码成本很低但听感提升非常明显。
 */

/** 把音程数组旋转 inv 次，越界的部分升八度 —— 这就是转位 */
function invertIntervals(intervals, inv) {
  const n = intervals.length;
  const out = [];
  for (let i = 0; i < n; i++) {
    const j = i + inv;
    out.push(intervals[j % n] + Math.floor(j / n) * 12);
  }
  return out.sort((a, b) => a - b);
}

function buildVoicing(chord, prevVoicing) {
  const intervals = chord.intervals;
  const n = intervals.length;

  let best = null;
  let bestCost = Infinity;

  for (let octave = 0; octave <= 1; octave++) {
    for (let inv = 0; inv < n; inv++) {
      const base = 48 + chord.root + octave * 12;
      const notes = invertIntervals(intervals, inv).map((iv) => base + iv);

      // 音域约束：伴奏保持在 A2..E5，别跑到太低或太高
      if (notes[0] < 45 || notes[notes.length - 1] > 76) continue;

      let cost;
      if (prevVoicing && prevVoicing.length) {
        cost = 0;
        const m = Math.min(notes.length, prevVoicing.length);
        for (let i = 0; i < m; i++) cost += Math.abs(notes[i] - prevVoicing[i]);
        // 声部数变化也惩罚一下，避免三和弦/七和弦来回换来换去
        cost += Math.abs(notes.length - prevVoicing.length) * 8;
      } else {
        // 第一个和弦：让最低音落在 C3 附近，像是个"起手式"
        cost = Math.abs(notes[0] - 52) * 2;
      }

      if (cost < bestCost) {
        bestCost = cost;
        best = notes;
      }
    }
  }

  return best ?? intervals.map((iv) => 48 + chord.root + iv);
}

/**
 * 编排：把 LLM 给的 4~8 小节和声「素材」，扩展成一首有段落结构的曲子。
 *
 * ★ 为什么要在后端做这件事，而不是让 LLM 直接给 20 小节的和弦：
 *   LLM 擅长给"有性格的和声素材"（一个 4 小节的走向），
 *   但不擅长规划曲式结构，而且让它吐 20 小节容易中途跑偏/重复自己。
 *   所以分工是：**LLM 出素材，后端做编排**。素材循环使用，
 *   靠"段落 + 配器密度 + 音区"制造变化。
 *
 * 结构（比例随总长度缩放）：
 *   intro  1 小节   只有琶音和低音，鼓很轻，不弹旋律   -> 建立氛围
 *   verse  ~45%     A 动机，鼓中等                    -> 陈述
 *   chorus ~35%     B 动机（新动机）+ 旋律升八度 + 鼓满 -> 高潮
 *   verse2 余下      A 动机回来                        -> 回归
 *   outro  1 小节   收尾，鼓很轻，不弹旋律              -> 句号
 */
function buildArrangement(totalBars) {
  if (totalBars < 8) {
    // 太短就退化成单一乐段，硬塞段落结构反而别扭
    return [{ name: 'main', bars: totalBars, motif: 'A', octave: 0, drums: 8, melody: 1 }];
  }

  const intro = 1;
  const outro = 1;
  const body = totalBars - intro - outro;
  const verse = Math.max(2, Math.round(body * 0.45));
  const chorus = Math.max(2, Math.round(body * 0.35));
  const verse2 = Math.max(0, body - verse - chorus);

  const secs = [
    { name: 'intro', bars: intro, motif: 'A', octave: 0, drums: 3, melody: 0 },
    { name: 'verse', bars: verse, motif: 'A', octave: 0, drums: 7, melody: 1 },
    { name: 'chorus', bars: chorus, motif: 'B', octave: 1, drums: 9, melody: 1 },
    { name: 'verse2', bars: verse2, motif: 'A', octave: 0, drums: 7, melody: 1 },
    { name: 'outro', bars: outro, motif: 'A', octave: 0, drums: 2, melody: 0 },
  ];
  return secs.filter((s) => s.bars > 0);
}

/**
 * 生成一个 1 小节的「动机」：节奏 + 级进走向。
 *
 * 动机是旋律能被记住的最小单位。整首曲子的旋律都由这两个动机
 * （A 和 B）反复发展而来 —— 这是"听起来像曲子"和"听起来像随机音效"的分界线。
 */
function makeMotif(rnd, stepsPerBar, stepsPerBeat) {
  // 节奏：强拍必发音（骨架），弱拍约 40% 概率发音（留呼吸）
  const hit = [];
  for (let i = 0; i < stepsPerBar; i++) {
    hit.push(i % stepsPerBeat === 0 ? true : rnd() < 0.4);
  }
  hit[0] = true;

  // 走向：级进为主（±1 级），偶尔跳进（±2 级）
  const move = new Array(stepsPerBar).fill(0);
  for (let i = 0; i < stepsPerBar; i++) {
    if (!hit[i]) continue;
    move[i] = rnd() < 0.62 ? (rnd() < 0.5 ? 1 : -1) : rnd() < 0.5 ? 2 : -2;
  }

  // 变化重复（A'）：把动机最后一个音的走向反过来，做出"似曾相识但不同"
  const moveVar = move.slice();
  for (let i = stepsPerBar - 1; i >= 0; i--) {
    if (hit[i]) {
      moveVar[i] = -move[i];
      break;
    }
  }

  // 收束节奏：只在"拍"上发音，节奏减半，音符变少 -> 听觉上"这句要结束了"
  const cadence = [];
  for (let i = 0; i < stepsPerBar; i++) cadence.push(i % stepsPerBeat === 0);
  cadence[stepsPerBar - 1] = true; // 最后一格必须有音，否则整段的"句号音"落在休止上

  return { hit, move, moveVar, cadence };
}

/**
 * 生成整首曲子的旋律 —— 由「动机 + 段落 + 变奏」驱动。
 *
 * 每个小节复用所在段落的动机节奏与走向，所以听众能听出"同一句在重复"；
 * 而强拍又吸附到当前小节的和弦音上，所以重复的同时不会和声打架。
 *
 * 返回值约定（★ ESP32 端依赖）：
 *   notes[i] === -1  第 i 格是休止，不发声
 *   notes[i] >= 0    第 i 格触发这个 MIDI 音
 */
function buildMelody({ keyIndex, scaleSteps, barChords, sections, stepsPerBar, stepsPerBeat, seed }) {
  const rnd = mulberry32(seed);
  const scaleLen = scaleSteps.length;
  const totalBars = sections.reduce((a, s) => a + s.bars, 0);

  const motifs = {
    A: makeMotif(rnd, stepsPerBar, stepsPerBeat),
    B: makeMotif(rnd, stepsPerBar, stepsPerBeat),
  };

  const notes = [];
  const hold = [];
  let barIndex = 0;

  for (const sec of sections) {
    for (let b = 0; b < sec.bars; b++, barIndex++) {
      const chord = barChords[barIndex];
      const motif = motifs[sec.motif] || motifs.A;

      // 前奏/尾奏不弹旋律，把空间让给琶音，制造"进入"和"离开"的感觉
      if (!sec.melody) {
        for (let s = 0; s < stepsPerBar; s++) {
          notes.push(-1);
          hold.push(1);
        }
        continue;
      }

      // 段落最后一小节用收束节奏；每 4 小节的第 2 小节用变化重复；其余用原型
      const isSectionEnd = b === sec.bars - 1;
      const hitArr = isSectionEnd ? motif.cadence : motif.hit;
      const moveArr = !isSectionEnd && b % 4 === 1 ? motif.moveVar : motif.move;

      // 每小节都从同一音级起步 —— 这样动机才是真的"重复"，
      // 而不是每次都漂到不同高度
      let degree = 0;

      for (let s = 0; s < stepsPerBar; s++) {
        const onBeat = s % stepsPerBeat === 0;
        const strong = s === 0 || s === 2 * stepsPerBeat;

        if (!hitArr[s]) {
          notes.push(-1);
          hold.push(1);
          continue;
        }

        degree = (((degree + moveArr[s]) % scaleLen) + scaleLen) % scaleLen;

        let midi;
        if (barIndex === totalBars - 1 && s === stepsPerBar - 1) {
          // 全曲最后一个音：落在最后一个和弦的根音上，给整首曲子一个明确的句号。
          // 因为可以循环播放，收在属和弦根音上正好接回主和弦，循环会很自然。
          midi = 60 + chord.root;
        } else if (strong) {
          // 强拍吸附到当前小节的和弦音
          const iv = chord.intervals[Math.floor(rnd() * chord.intervals.length)];
          midi = 60 + chord.root + iv;
        } else {
          midi = 60 + keyIndex + scaleSteps[degree];
        }

        if (sec.octave) midi += 12; // 副歌升八度 -> 情绪抬起来

        // 副歌的音域整体上移，避免被 fitRange 又拉回中音区
        const lo = sec.octave ? 72 : 60;
        const hi = sec.octave ? 96 : 84;
        notes.push(fitRange(midi, lo, hi));
        hold.push(onBeat ? 2 : 1);
      }
    }
  }

  return { notes, hold };
}

/**
 * 主入口：把「音乐意图」展开成设备可直接使用的数字层 + 人类可读的标签层。
 *
 * 设计原则：**设备只读 music，人只读 labels。**
 * ESP32 端完全忽略 labels 字段，可以省掉 JSON 解析的很多麻烦。
 *
 * @param {object} intent
 * @param {number|string} [intent.key]        调式主音，0-11 或 "C"/"F#"/"Bb"
 * @param {string}        [intent.scale]      音阶名，如 "major" / "minor"
 * @param {number}        [intent.bpm]        速度
 * @param {string[]}      [intent.chords]     和弦符号数组，如 ["Cmaj7","Am7"]
 * @param {number}        [intent.bars]       小节数
 * @param {string}        [intent.arp]        伴奏音型名，见 ARP_PATTERNS
 * @param {string}        [intent.instrument] 音色名
 * @param {string}        [intent.style]      风格标签（仅用于 labels）
 * @param {string}        [intent.seed_text]  随机种子来源（一般传识别文本）
 */
export function expandMusicIntent(intent = {}) {
  const bpm = clampInt(intent.bpm, 40, 220, 90);
  const keyIndex = pitchClass(intent.key);
  const scaleIndex = findScaleIndex(intent.scale);
  const scaleSteps = SCALES[scaleIndex].steps;

  // --- 和声素材（LLM 给的 4~8 小节走向）---
  let chordSymbols = Array.isArray(intent.chords)
    ? intent.chords.map((c) => String(c ?? '').trim()).filter(Boolean)
    : [];
  if (chordSymbols.length === 0) chordSymbols = defaultProgression(keyIndex, scaleIndex);

  const beatsPerBar = clampInt(intent.beats_per_bar, 2, 8, 4);
  const stepsPerBeat = clampInt(intent.steps_per_beat, 1, 4, 2);

  // 素材解析：最多用 8 小节，多的截掉
  const prog = chordSymbols.slice(0, 8).map((sym) => {
    const parsed = parseChord(sym);
    return (
      parsed ?? { root: keyIndex, typeIndex: 0, intervals: CHORD_TYPES[0].intervals, symbol: sym }
    );
  });

  // --- 编排：把素材扩展成一首有段落结构的曲子 ---
  // total_bars 由调用方通过环境变量决定，默认 16 小节（90bpm 下约 42 秒）
  const bars = clampInt(intent.total_bars, 4, 32, 16);
  const sections = buildArrangement(bars);

  // 每小节的和弦 = 素材循环。所以 LLM 只需要给 4 小节"有性格的"走向，
  // 编排交给后端 —— 这也是为什么不让 LLM 直接吐 20 小节和弦
  const chords = [];
  for (let i = 0; i < bars; i++) chords.push(prog[i % prog.length]);

  // --- 音色 ---
  const instrumentIndex = findInstrumentIndex(intent.instrument);
  const instrument = INSTRUMENTS[instrumentIndex];

  // --- 伴奏音型 ---
  const arpKey = String(intent.arp ?? '').trim().toLowerCase().replace(/[\s-]+/g, '_');
  let arpPattern = ARP_PATTERNS[arpKey];
  if (!arpPattern) {
    // 没有指定就按音阶性质挑一个合理的
    const isMinorish = scaleIndex === 1 || scaleIndex === 2 || scaleIndex === 6 || scaleIndex === 7;
    arpPattern = isMinorish ? ARP_PATTERNS.arp_wave : ARP_PATTERNS.fingerpick;
  }

  // 扫弦：应该【一次同时拨响所有和弦音】，而不是把同一个音连弹几次。
  // 原实现把 strum 当成 "arp_pattern = [0,0,0,0]"，结果就是根音连弹 4 下 —— 听着很怪。
  const strum = arpKey === 'strum' || intent.strum === true;

  // --- 和弦声位（带声部连接） ---
  const chordNotes = [];
  const chordNoteCounts = [];
  let prevVoicing = null;
  for (const c of chords) {
    const voicing = buildVoicing(c, prevVoicing);
    chordNotes.push(...voicing);
    chordNoteCounts.push(voicing.length);
    prevVoicing = voicing;
  }

  // --- 低音线：每小节一个根音（低两个八度）---
  // 加上低音之后整段音乐立刻「有底」，是听感提升最明显的一步之一
  const bassNotes = chords.map((c) => {
    let n = 36 + c.root; // 从 C2 开始
    while (n < 36) n += 12;
    while (n > 47) n -= 12;
    return n;
  });

  // --- 旋律 ---
  const seed = hashString(
    `${intent.seed_text ?? ''}|${keyIndex}|${scaleIndex}|${bpm}|${bars}`
  );
  const melody = buildMelody({
    keyIndex,
    scaleSteps,
    barChords: chords,
    sections,
    stepsPerBar: beatsPerBar * stepsPerBeat,
    stepsPerBeat,
    seed,
  });

  // --- 每小节的鼓点强度 0~9（制造段落动态）---
  // 前奏/尾奏鼓轻、副歌鼓满。没有这个的话整首曲子的力度就是一条直线。
  const drumLevel = [];
  for (const sec of sections) {
    for (let b = 0; b < sec.bars; b++) drumLevel.push(sec.drums);
  }

  // --- 鼓点 ---
  // intent.drums 可以是 'off' / 'auto'；默认 auto（按风格自动挑）
  const drumsMode = String(intent.drums ?? 'auto').toLowerCase();
  const drumKey = drumsMode === 'off' ? 'none' : pickDrumPattern(intent.style, instrumentIndex);
  const drumPattern = DRUM_PATTERNS[drumKey] ?? DRUM_PATTERNS.none;

  const stepMs = Math.round(60000 / bpm / stepsPerBeat);

  return {
    music: {
      bpm,
      key_index: keyIndex,
      scale_index: scaleIndex,
      bars,
      beats_per_bar: beatsPerBar,
      steps_per_beat: stepsPerBeat,
      step_ms: stepMs, // 预计算好，设备端连除法都不用做
      chord_notes: chordNotes,
      chord_note_counts: chordNoteCounts,
      bass_notes: bassNotes, // 每小节一个低音（低两个八度），让音乐"有底"
      arp_pattern: arpPattern,
      strum, // true = 每个音型步一次性拨响所有和弦音（扫弦）
      strum_spread_ms: strum ? 22 : 0, // 扫弦时各弦之间的错开毫秒数，模拟手指扫过
      melody_notes: melody.notes, // -1 = 休止
      melody_hold: melody.hold, // 每个音保持多少格
      instrument_index: instrumentIndex,
      decay_seconds: instrument.decay_seconds, // 衰减到 -60dB 的秒数，见文件上方推导
      brightness: instrument.brightness,
      damping: instrument.damping, // 扩展 KS：环路阻尼（高频消失速度，独立于总延音）
      pick_position: instrument.pick_position, // 扩展 KS：拨弦位置 0..0.5

      // --- 鼓点（网格：每小节 16 个 16 分音符，与 steps_per_beat 无关）---
      drum_pattern: drumKey, // 只是给人看的标签
      drum_step_ms: Math.round(60000 / bpm / 4), // 鼓点每格的毫秒数
      drum_kick: drumPattern.kick, // 每格 0/1
      drum_snare: drumPattern.snare,
      drum_hat: drumPattern.hat,
      drum_level: drumLevel, // 每小节的鼓点强度 0~9（段落动态）

      // --- 曲式结构（设备端不需要，是给人看和给前端画结构的）---
      sections: sections.map((s) => ({ name: s.name, bars: s.bars })),
    },
    labels: {
      key: PITCH_NAMES[keyIndex],
      scale: SCALES[scaleIndex].key,
      // 这里给的是「和声素材」（LLM 给的那 4 小节走向），不是每小节展开后的列表。
      // 16 小节里同一组和弦会重复 4 遍，全列出来对人来说是噪音。
      chords: prog.map((c) => c.symbol),
      instrument: instrument.key,
      style: String(intent.style ?? 'pop'),
      // 给人看/给前端画结构的曲式
      form: sections.map((s) => `${s.name}×${s.bars}`).join(' - '),
    },
  };
}
