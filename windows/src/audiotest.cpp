/*
 * audiotest.exe —— 「原生分析链」的确定性对照台（离线，不出声、不渲染）。
 *
 * 用法:
 *   audiotest.exe <dump.csv> <out.csv> <fps> [label]
 *
 * 它做什么：
 *   把 光柱.exe --dumpspec 录下来的 128 个频谱值（固定 30Hz）按**理想渲染循环**
 *   重新播一遍，每帧导出 16 个平滑量。同一份 dump 交给
 *   tools/orig-analyzer.js（从原作品 bundle 逐字抠出来的 getAudioData）跑，
 *   两边逐帧 diff。
 *
 * 为什么必须有这个东西：
 *   用户报「原生观感比 Electron 差、响应幅度偏小、部分节拍不响应」。
 *   这种「感觉」类的偏差靠肉眼比对是查不出来的 —— 振幅差 30% 你看着只是
 *   「好像没那么跳」，而问题可能出在平滑系数、调用频率、频段映射、上采样
 *   里的任何一处。必须把同一串输入喂给两条链，逐帧比数字，差多少就是多少。
 *
 * 循环的语义（两边必须严格一致，否则比出来的差全是自己的模拟误差）：
 *
 *   dt = 1/fps,  specDt = 1/30
 *   specAccum = 0,  row = 0,  cur = 全 0（两边的初始状态都是 0）
 *   while (row < 行数):
 *       specAccum += dt
 *       while (specAccum >= specDt && row < 行数):
 *           specAccum -= specDt
 *           cur = rows[row++]
 *       analyze(cur)          ← 等价于 LpAudio::tick() 末尾的那一次 analyze512()
 *       导出 16 个值
 *
 *   注意第一帧用的是**全 0**（specAccum 还没攒够 1/30），和原生 tick() 一致。
 *   频谱层的 30Hz 是硬编码的，和 fps 无关 —— 这正是本轮要守住的结构。
 *
 * 退出码：0 成功；1 参数/文件错。
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio.h"

/* 16 个量的名字与顺序 —— 必须和 tools/orig-analyzer.js 的导出顺序一致 */
static const char* kNames[16] = {
    "bass", "mid", "treble", "energy", "subBass", "lowMid", "highMid",
    "presence", "brilliance", "air", "warmth", "brightness", "sharpness",
    "smoothness", "density", "spectralCentroid"};

struct Row {
  std::string label;
  double v[LP_OUT_BINS];
};

/* 读 dump。行格式：<label>,<v0>,<v1>,...,<v127>
   '#' 开头是注释（startDump 写的那行头）。 */
static bool loadDump(const char* path, std::vector<Row>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "[audiotest] 打不开 dump: %s\n", path);
    return false;
  }
  char line[16384];
  long bad = 0;
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n == 0 || line[0] == '#') continue;

    Row r;
    char* p = line;
    char* comma = strchr(p, ',');
    if (!comma) {
      bad++;
      continue;
    }
    *comma = 0;
    r.label = p;
    p = comma + 1;

    int got = 0;
    while (got < LP_OUT_BINS) {
      char* end = nullptr;
      const double v = strtod(p, &end);
      if (end == p) break;
      r.v[got++] = v;
      if (!*end) break;
      p = end + 1;
    }
    if (got != LP_OUT_BINS) {
      bad++;
      continue;
    }
    out->push_back(std::move(r));
  }
  fclose(f);
  fprintf(stderr, "[audiotest] 读入 %ld 行（跳过 %ld 行不合格式）\n",
          (long)out->size(), bad);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr,
            "用法: audiotest.exe <dump.csv> <out.csv> <fps> [label]\n"
            "  fps   : 模拟的渲染帧率（= 原作品的 FPS_LIMIT）\n"
            "  label : 只跑 dump 里这个标签的行；省略则跑全部\n");
    return 1;
  }
  const char* dumpPath = argv[1];
  const char* outPath = argv[2];
  const double fps = atof(argv[3]);
  /* ⚠ 空字符串要当成「没有给标签」，不能当成「标签就是空串」。
     脚本里写 `... $hz "$LABEL"`，LABEL 为空时**仍然会传一个空参数进来**，
     于是变成「只跑标签为空的行」—— 一行都匹配不到，输出文件根本不生成，
     而报错只有一句「没有匹配的行」，很难看出是这么回事。 */
  const char* wantLabel = (argc >= 5 && argv[4][0] != '\0') ? argv[4] : nullptr;

  if (fps < 1.0 || fps > 1000.0) {
    fprintf(stderr, "[audiotest] fps 不合理: %g\n", fps);
    return 1;
  }

  std::vector<Row> all;
  if (!loadDump(dumpPath, &all)) return 1;

  std::vector<const Row*> rows;
  for (const Row& r : all)
    if (!wantLabel || r.label == wantLabel) rows.push_back(&r);
  if (rows.empty()) {
    fprintf(stderr, "[audiotest] 没有匹配的行（label=%s）\n",
            wantLabel ? wantLabel : "<全部>");
    fprintf(stderr, "[audiotest]   dump 里的标签看第一行 `#spec128 label=...`；\n"
                    "[audiotest]   传空字符串会被当成「标签是空串」，这里已经当没给处理了。\n");
    return 1;
  }
  fprintf(stderr, "[audiotest] 取 %ld 帧频谱，按 %.4g fps 重放\n",
          (long)rows.size(), fps);

  FILE* out = fopen(outPath, "wb");
  if (!out) {
    fprintf(stderr, "[audiotest] 打不开输出: %s\n", outPath);
    return 1;
  }
  /* 表头第一列是 frame（从 0 开始），其余 16 列 = 16 个平滑量 */
  fputs("frame", out);
  for (int i = 0; i < 16; i++) fprintf(out, ",%s", kNames[i]);
  fputc('\n', out);

  LpAudio a;
  a.resetTrackers();

  const double dt = 1.0 / fps;
  const double specDt = 1.0 / 30.0;   // ★ 频谱层固定 30Hz，和 fps 无关
  double specAccum = 0.0;
  double cur[LP_OUT_BINS];
  for (int i = 0; i < LP_OUT_BINS; i++) cur[i] = 0.0;

  long row = 0, frame = 0;
  const long nrows = (long)rows.size();
  while (row < nrows) {
    specAccum += dt;
    /* 一次最多追 4 帧（和 tick() 里的 guard 同一个意思） */
    int guard = 0;
    while (specAccum >= specDt && row < nrows && guard++ < 4) {
      specAccum -= specDt;
      memcpy(cur, rows[row]->v, sizeof(cur));
      row++;
    }
    a.testFeedSpectrum(cur);

    const double* s = a.smoothed();
    fprintf(out, "%ld", frame);
    for (int i = 0; i < 16; i++) fprintf(out, ",%.9f", s[i]);
    fputc('\n', out);
    frame++;
  }

  fclose(out);
  fprintf(stderr, "[audiotest] 写出 %ld 帧 → %s\n", frame, outPath);
  return 0;
}
