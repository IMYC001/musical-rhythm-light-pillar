/*
 * jsref.js —— 「参考实现分析链」的确定性对照台（Node，离线）。
 *
 * 用法:
 *   node tools/jsref.js <dump.csv> <out.csv> <fps> [label]
 *
 * 参数和循环语义与 src/audiotest.cpp **完全一致**（见那边的注释），
 * 唯一的区别是这里的分析器是 tools/ref-analyzer.js ——
 * 冻结在版本库里的参考实现，不是照着手抄的复刻。
 * 所以两边 diff 出来的差值就是真实的实现差异，不掺「我抄错了」的噪声。
 *
 * 为什么 fps 要作为参数：
 *   参考实现 `getAudioData` 跑在定时器里（bundle 里是 setInterval(ht, 1e3/1e3)），
 *   而它里面的一阶平滑系数 `ee = r>0 ? .15 : .08` 是**每次调用作用一次**、
 *   **不按 dt 缩放**的。所以「调用频率」直接决定平滑的时间常数 ——
 *   这恰恰是本轮要查的东西，必须能扫着试。
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { AudioAnalyzer } = require(path.join(__dirname, 'ref-analyzer.js'));

const NAMES = ['bass', 'mid', 'treble', 'energy', 'subBass', 'lowMid', 'highMid',
  'presence', 'brilliance', 'air', 'warmth', 'brightness', 'sharpness',
  'smoothness', 'density', 'spectralCentroid'];

function main() {
  const [, , dumpPath, outPath, fpsArg, labelArg] = process.argv;
  if (!dumpPath || !outPath || !fpsArg) {
    console.error('用法: node tools/jsref.js <dump.csv> <out.csv> <fps> [label]');
    process.exit(1);
  }
  const fps = parseFloat(fpsArg);
  if (!(fps >= 1 && fps <= 1000)) {
    console.error('[jsref] fps 不合理: ' + fpsArg);
    process.exit(1);
  }

  const text = fs.readFileSync(dumpPath, 'utf8');
  const rows = [];
  let bad = 0;
  for (const raw of text.split('\n')) {
    const line = raw.replace(/\r$/, '');
    if (!line || line[0] === '#') continue;
    const parts = line.split(',');
    const label = parts.shift();
    if (labelArg && label !== labelArg) continue;
    if (parts.length !== 128) { bad++; continue; }
    rows.push({ label, v: parts.map(Number) });
  }
  console.error('[jsref] 读入 ' + rows.length + ' 行（跳过 ' + bad + ' 行不合格式）');
  if (!rows.length) {
    console.error('[jsref] 没有匹配的行（label=' + (labelArg || '<全部>') + '）');
    process.exit(1);
  }
  console.error('[jsref] 取 ' + rows.length + ' 帧频谱，按 ' + fps + ' fps 重放');

  const a = new AudioAnalyzer();

  const dt = 1 / fps;
  const specDt = 1 / 30;          // ★ 频谱层固定 30Hz，和 fps 无关
  let specAccum = 0;
  let cur = new Array(128).fill(0);   // 两边的初始状态都是 0

  const out = [];
  out.push('frame,' + NAMES.join(','));
  let row = 0, frame = 0;
  while (row < rows.length) {
    specAccum += dt;
    let guard = 0;
    while (specAccum >= specDt && row < rows.length && guard++ < 4) {
      specAccum -= specDt;
      cur = rows[row].v;
      row++;
    }
    /* 桥接层每帧都会调 setAudioFrame（哪怕频谱没变），
       参考实现内部再做 128→512 上采样。这里照做，语义和原生一致。 */
    a.setAudioFrame(cur);
    const d = a.getAudioData(dt);

    const vals = NAMES.map((n) => (typeof d[n] === 'number' ? d[n] : 0));
    out.push(frame + ',' + vals.map((v) => v.toFixed(9)).join(','));
    frame++;
  }

  fs.writeFileSync(outPath, out.join('\n') + '\n');
  console.error('[jsref] 写出 ' + frame + ' 帧 → ' + outPath);
}

main();
