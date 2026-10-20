/*
 * 从作品 bundle 里抽出主题色表，生成 C++ 头文件。
 *
 * 为什么用抽的而不是手抄：主题色是「线性」色彩空间的值（Three 的 Color），
 * 抄错一个小数点整根光柱的颜色就偏了，而且 15 套主题 × 4 个颜色 = 60 个数。
 * 直接正则抽出来最稳，而且原作品改了主题这张表也能跟着更新。
 */
const fs = require('fs');
const path = require('path');

const BUNDLE = process.env.LP_BUNDLE;
if (!BUNDLE) {
  console.error(
    '需要指定原作品的 JS bundle：\n' +
    '  LP_BUNDLE=/path/to/assets/index-XXXX.js node scripts/gen_data.js\n' +
    '\n' +
    '（src/data_themes.h 已经进版本库，日常构建不需要跑这一步。）'
  );
  process.exit(1);
}
const OUT = path.join(__dirname, '..', 'src', 'data_themes.h');

const src = fs.readFileSync(BUNDLE, 'utf8');

/* ---- 1. 主题键名列表（决定面板里的顺序）---- */
const keysMatch = src.match(/\[("(?:nocturnal|ocean-deep)[^\]]*)\]/);
if (!keysMatch) throw new Error('找不到主题键名列表');
const order = JSON.parse('[' + keysMatch[1] + ']');

/* ---- 2. 主题定义块：从第一个 nocturnal 定义开始做大括号配平 ---- */
const startIdx = src.indexOf('nocturnal:{name:');
if (startIdx < 0) throw new Error('找不到主题定义起点');
// 往左退到包含它的 '{'
let objStart = src.lastIndexOf('{', startIdx);
let depth = 0;
let end = -1;
for (let i = objStart; i < src.length; i++) {
  const ch = src[i];
  if (ch === '{') depth++;
  else if (ch === '}') {
    depth--;
    if (depth === 0) { end = i; break; }
  }
}
if (end < 0) throw new Error('主题定义块没有配平');
const block = src.slice(objStart, end + 1);

/* ---- 3. 逐个主题抽字段 ----
   注意：主题对象里字段是**交错**的（uBaseColor1, uBaseColor2, uCoolCore,
   uCoolEdge, uWarmCore, uWarmEdge, uRippleColor, uPeakColor, uGlowIntensity），
   所以不能用一个「四个颜色连在一起」的正则去匹配，要按主题切片后逐字段找。 */
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/* 把整块按顶层逗号切成每个主题的片段 */
function splitThemes(blockText) {
  const inner = blockText.slice(1, -1); // 去掉最外层大括号
  const parts = [];
  let depth = 0;
  let cur = '';
  for (const ch of inner) {
    if (ch === '{') depth++;
    else if (ch === '}') depth--;
    if (ch === ',' && depth === 0) {
      parts.push(cur);
      cur = '';
      continue;
    }
    cur += ch;
  }
  if (cur.trim()) parts.push(cur);
  return parts;
}

const pieces = splitThemes(block);
const byKey = new Map();
for (const p of pieces) {
  const k = p.match(/^\s*"?([\w-]+)"?\s*:/);
  if (k) byKey.set(k[1], p);
}

const themes = [];
for (const key of order) {
  const piece = byKey.get(key);
  if (!piece) throw new Error('找不到主题片段: ' + key);

  const name = piece.match(/name:"([^"]+)"/);
  if (!name) throw new Error('找不到主题名: ' + key);

  const vec = (field) => {
    const m = piece.match(
      new RegExp(esc(field) + ':new\\s+\\w+\\(\\s*([-\\d.]+)\\s*,\\s*([-\\d.]+)\\s*,\\s*([-\\d.]+)\\s*\\)')
    );
    if (!m) throw new Error(`主题 ${key} 缺字段 ${field}`);
    return [Number(m[1]), Number(m[2]), Number(m[3])];
  };
  const num = (field) => {
    const m = piece.match(new RegExp(esc(field) + ':\\s*([-\\d.]+)'));
    if (!m) throw new Error(`主题 ${key} 缺字段 ${field}`);
    return Number(m[1]);
  };

  themes.push({
    key,
    name: name[1],
    base: vec('uBaseColor1'),
    cool: vec('uCoolCore'),
    warm: vec('uWarmCore'),
    peak: vec('uPeakColor'),
    glow: num('uGlowIntensity'),
  });
}

/* ---- 4. 生成头文件 ---- */
const f3 = (a) => `{${a.map((v) => v.toFixed(6)).join('f, ')}f}`;

const lines = [];
lines.push('// 由 scripts/gen_data.js 从作品 bundle 自动抽取，请勿手改。');
lines.push('// 数值是 Three Color 的**线性**值，着色器里会过一遍 lin2srgb。');
lines.push('#pragma once');
lines.push('');
lines.push('struct LpTheme {');
lines.push('  const char* key;');
lines.push('  const char* name;');
lines.push('  float base[3];   // uBaseColor1（只用于“主题底色”，覆盖层不画它）');
lines.push('  float cool[3];   // uCoolCore');
lines.push('  float warm[3];   // uWarmCore');
lines.push('  float peak[3];   // uPeakColor');
lines.push('  float glow;      // uGlowIntensity');
lines.push('};');
lines.push('');
lines.push(`static const int LP_THEME_COUNT = ${themes.length};`);
lines.push('static const LpTheme LP_THEMES[] = {');
for (const t of themes) {
  lines.push(
    `  {"${t.key}", "${t.name}", ${f3(t.base)}, ${f3(t.cool)}, ${f3(t.warm)}, ${f3(t.peak)}, ${t.glow.toFixed(4)}f},`
  );
}
lines.push('};');
lines.push('');

fs.writeFileSync(OUT, lines.join('\n'), 'utf8');

console.log(`[gen_data] ${themes.length} 套主题 → ${path.relative(process.cwd(), OUT)}`);
for (const t of themes) {
  console.log(
    `           ${t.key.padEnd(20)} ${t.name}  cool=[${t.cool.join(',')}] peak=[${t.peak.join(',')}] glow=${t.glow}`
  );
}
