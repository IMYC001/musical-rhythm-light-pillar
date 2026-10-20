/*
 * make-icon.js —— 生成托盘图标与 exe 图标
 *
 * 不引入任何图形库：自己按光柱的造型算式画一张 RGBA 位图，再手写 PNG / ICO 封装。
 * 造型刻意用和片元着色器同一套「到竖直轴段的距离场」，所以图标和画面是同一个东西。
 *
 * 托盘用 16/32 的 PNG；exe 用多尺寸 ICO（Windows 从 Vista 起支持 ICO 里直接放 PNG）。
 */
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const OUT_DIR = path.resolve(__dirname, '..', 'assets');

/* ---------------- 造型 ---------------- */
const INSET = 0.205;      // 轴心离两边的距离（图标里稍微内收，别被切掉）
const HALF_W = 0.115;     // 光晕半宽
const TOP_H = 0.775;      // 柱高（自下而上）
const COOL = [126, 92, 255];
const WARM = [236, 184, 255];

function smoothstep(a, b, x) {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

function render(size) {
  const buf = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) {
    const v = 1 - (y + 0.5) / size;              // 自下而上
    for (let x = 0; x < size; x++) {
      const u = (x + 0.5) / size;
      let a = 0;
      for (let side = 0; side < 2; side++) {
        const e = (side === 0 ? u : 1 - u) - INSET;
        const dx = e;
        const dy = Math.max(0, v - TOP_H);
        const d = Math.sqrt(dx * dx + dy * dy);
        const g = 1 - smoothstep(0, HALF_W, d);
        const grad = 0.5 + 0.5 * Math.min(1, Math.max(0, v / TOP_H));
        a += g * grad;
      }
      a = Math.min(1, a * 1.18);
      const t = Math.min(1, Math.max(0, v / TOP_H));
      const r = Math.round(COOL[0] + (WARM[0] - COOL[0]) * t);
      const gg = Math.round(COOL[1] + (WARM[1] - COOL[1]) * t);
      const b = Math.round(COOL[2] + (WARM[2] - COOL[2]) * t);
      const o = (y * size + x) * 4;
      buf[o] = r; buf[o + 1] = gg; buf[o + 2] = b;
      buf[o + 3] = Math.round(a * 255);
    }
  }
  return buf;
}

/* ---------------- PNG ---------------- */
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}

function encodePNG(size, rgba) {
  const stride = size * 4;
  const raw = Buffer.alloc((stride + 1) * size);
  for (let y = 0; y < size; y++) {
    raw[y * (stride + 1)] = 0;                                   // filter: none
    rgba.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0))
  ]);
}

/* ---------------- ICO ---------------- */
function encodeICO(entries) {
  const n = entries.length;
  const head = Buffer.alloc(6);
  head.writeUInt16LE(0, 0);
  head.writeUInt16LE(1, 2);
  head.writeUInt16LE(n, 4);
  const dirs = [];
  let offset = 6 + n * 16;
  for (const e of entries) {
    const d = Buffer.alloc(16);
    d[0] = e.size >= 256 ? 0 : e.size;
    d[1] = e.size >= 256 ? 0 : e.size;
    d.writeUInt16LE(1, 4);
    d.writeUInt16LE(32, 6);
    d.writeUInt32LE(e.png.length, 8);
    d.writeUInt32LE(offset, 12);
    offset += e.png.length;
    dirs.push(d);
  }
  return Buffer.concat([head, ...dirs, ...entries.map((e) => e.png)]);
}

/* ---------------- 输出 ---------------- */
fs.mkdirSync(OUT_DIR, { recursive: true });

const png256 = encodePNG(256, render(256));
fs.writeFileSync(path.join(OUT_DIR, 'icon.png'), png256);
fs.writeFileSync(path.join(OUT_DIR, 'icon-32.png'), encodePNG(32, render(32)));

const sizes = [16, 24, 32, 48, 64, 128, 256];
fs.writeFileSync(path.join(OUT_DIR, 'icon.ico'), encodeICO(
  sizes.map((s) => ({ size: s, png: s === 256 ? png256 : encodePNG(s, render(s)) }))
));

console.log('[make-icon] assets/icon.png  ' + png256.length + ' B');
console.log('[make-icon] assets/icon.ico  ' + fs.statSync(path.join(OUT_DIR, 'icon.ico')).size + ' B  (' + sizes.join('/') + ')');
