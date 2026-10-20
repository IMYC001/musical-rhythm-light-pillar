/*
 * 两张 PNG 的逐像素对比。
 *
 * 为什么要它：覆盖层是极低 alpha 的加光（最大 alpha 只有 11/255），
 * 抓屏之后缩略看图根本分辨不出来「到底叠上去了没有」。
 * 这个工具给出客观数字：变化像素占比、平均/最大色差、变化区域包围盒。
 * 和 Electron 版自检里的 diffImages 是同一套判据，便于两边对照。
 *
 * 用法：cmp.exe a.png b.png
 */
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Img {
  int w = 0, h = 0;
  std::vector<unsigned char> px;  // BGRA
  bool load(const wchar_t* path) {
    Gdiplus::Bitmap bmp(path);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    w = (int)bmp.GetWidth();
    h = (int)bmp.GetHeight();
    Gdiplus::Rect r(0, 0, w, h);
    Gdiplus::BitmapData bd;
    if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
      return false;
    px.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
      memcpy(&px[(size_t)y * w * 4], (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride,
             (size_t)w * 4);
    }
    bmp.UnlockBits(&bd);
    return true;
  }
};

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int argc = 0;
  /* ⚠ 用宽字符取参数：中文路径走 ANSI 的 argv 会变乱码，连文件都打不开 */
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 3) {
    printf("用法: cmp.exe a.png b.png\n");
    return 1;
  }

  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

  Img a, b;
  if (!a.load(argv[1])) { wprintf(L"[cmp] 读不了 %ls\n", argv[1]); return 1; }
  if (!b.load(argv[2])) { wprintf(L"[cmp] 读不了 %ls\n", argv[2]); return 1; }
  printf("[cmp] A=%dx%d  B=%dx%d\n", a.w, a.h, b.w, b.h);
  if (a.w != b.w || a.h != b.h) { printf("[cmp] 尺寸不一致\n"); return 1; }

  long long changed = 0;
  long long sumDelta = 0;
  int maxDelta = 0, maxX = -1, maxY = -1;
  int bx0 = a.w, by0 = a.h, bx1 = -1, by1 = -1;
  for (int y = 0; y < a.h; y++) {
    for (int x = 0; x < a.w; x++) {
      const unsigned char* p = &a.px[((size_t)y * a.w + x) * 4];
      const unsigned char* q = &b.px[((size_t)y * a.w + x) * 4];
      int d = 0;
      for (int c = 0; c < 3; c++) {   // 只比 BGR，alpha 抓屏后不可靠
        int t = abs((int)p[c] - (int)q[c]);
        if (t > d) d = t;
      }
      if (d > 2) {
        changed++;
        sumDelta += d;
        if (d > maxDelta) { maxDelta = d; maxX = x; maxY = y; }
        if (x < bx0) bx0 = x;
        if (y < by0) by0 = y;
        if (x > bx1) bx1 = x;
        if (y > by1) by1 = y;
      }
    }
  }
  const double pct = 100.0 * (double)changed / ((double)a.w * a.h);
  printf("[cmp] 变化像素 = %lld (%.2f%%)  平均色差 = %.1f  最大色差 = %d @ (%d,%d)\n",
         changed, pct, changed ? (double)sumDelta / changed : 0.0, maxDelta, maxX, maxY);
  if (bx1 >= 0) {
    printf("[cmp] 变化区域 bbox = (%d,%d)-(%d,%d)  宽%d 高%d\n", bx0, by0, bx1, by1,
           bx1 - bx0 + 1, by1 - by0 + 1);
  } else {
    printf("[cmp] 没有任何变化\n");
  }

  /* 采样左右两侧、底部区域的色差，确认是不是「两侧对称的光柱」 */
  for (int i = 0; i < 3; i++) {
    const int y = a.h - 20 - i * (a.h / 6);
    printf("[cmp] y=%4d: ", y);
    const int xs[] = {2, a.w / 8, a.w / 2, a.w * 7 / 8, a.w - 3};
    for (int j = 0; j < 5; j++) {
      const unsigned char* p = &a.px[((size_t)y * a.w + xs[j]) * 4];
      const unsigned char* q = &b.px[((size_t)y * a.w + xs[j]) * 4];
      printf("x%4d:(%3d→%3d,%3d→%3d,%3d→%3d) ", xs[j], p[2], q[2], p[1], q[1], p[0], q[0]);
    }
    printf("\n");
  }

  Gdiplus::GdiplusShutdown(token);
  return 0;
}
