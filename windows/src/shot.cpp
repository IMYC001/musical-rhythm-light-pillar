/*
 * 图像工具（GDI+）。
 *
 * 为什么要自己写：沙箱禁止 PowerShell 的 Add-Type（等于禁止 .NET 截屏），
 * 而验证透明覆盖层又必须看真实合成结果。反正已经有编译器了，写一个最省事。
 *
 * 用法：
 *   shot.exe <输出路径.png> [延迟毫秒]      抓全屏（含虚拟屏幕所有显示器）
 *   shot.exe --convert <输入图> <输出.png>  任意 GDI+ 能读的格式转 PNG
 *                                           （自检回读存的是 BMP，转成 PNG 才能看）
 *
 * ⚠ 参数走 CommandLineToArgvW 取宽字符：中文路径用 ANSI 的 argv 会变成乱码。
 */
#include <windows.h>
/* GDI+ 的头文件用到 PROPID，而它定义在 objidl/propidl 里。
   注意**不能**开 WIN32_LEAN_AND_MEAN —— 那会把 objidl 一起排掉，
   报一堆 "unknown type name 'PROPID'"。 */
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>

static bool findPngEncoder(CLSID* out) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (!size) return false;
  auto* info = (Gdiplus::ImageCodecInfo*)malloc(size);
  if (!info) return false;
  Gdiplus::GetImageEncoders(num, size, info);
  bool found = false;
  for (UINT i = 0; i < num; i++) {
    if (wcscmp(info[i].MimeType, L"image/png") == 0) {
      *out = info[i].Clsid;
      found = true;
      break;
    }
  }
  free(info);
  return found;
}

static int doConvert(const wchar_t* in, const wchar_t* out) {
  Gdiplus::Bitmap src(in);
  if (src.GetLastStatus() != Gdiplus::Ok) {
    printf("[shot] 读不了输入图\n");
    return 1;
  }
  CLSID png;
  if (!findPngEncoder(&png)) {
    printf("[shot] 找不到 PNG 编码器\n");
    return 1;
  }
  const Gdiplus::Status st = src.Save(out, &png, nullptr);
  if (st != Gdiplus::Ok) {
    printf("[shot] 保存失败, status=%d\n", (int)st);
    return 1;
  }
  printf("[shot] 已转换 %ux%u\n", src.GetWidth(), src.GetHeight());
  return 0;
}

int main() {
  /* 无缓冲：段错误时缓冲区里的输出会一起丢掉，看不到崩在哪一步 */
  setvbuf(stdout, nullptr, _IONBF, 0);

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 2) {
    printf("用法:\n  shot.exe <输出路径.png> [延迟毫秒]\n"
           "  shot.exe --convert <输入图> <输出.png>\n");
    return 1;
  }

  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  if (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) != Gdiplus::Ok) {
    printf("[shot] GDI+ 初始化失败\n");
    return 1;
  }
  int rc = 1;

  if (wcscmp(argv[1], L"--convert") == 0) {
    if (argc < 4) {
      printf("[shot] --convert 需要两个路径\n");
    } else {
      rc = doConvert(argv[2], argv[3]);
    }
    Gdiplus::GdiplusShutdown(token);
    LocalFree(argv);
    return rc;
  }

  const wchar_t* out = argv[1];
  const int delay = argc > 2 ? _wtoi(argv[2]) : 0;
  if (delay > 0) Sleep((DWORD)delay);

  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  printf("[shot] 虚拟屏幕 %dx%d @ %d,%d\n", w, h, x, y);
  if (w <= 0 || h <= 0) {
    printf("[shot] 拿不到虚拟屏幕尺寸，放弃\n");
    Gdiplus::GdiplusShutdown(token);
    LocalFree(argv);
    return 1;
  }

  HDC screen = GetDC(nullptr);
  if (!screen) {
    printf("[shot] GetDC(nullptr) 失败（沙箱可能不允许访问屏幕）\n");
    Gdiplus::GdiplusShutdown(token);
    LocalFree(argv);
    return 1;
  }
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  printf("[shot] mem=%p bmp=%p\n", (void*)mem, (void*)bmp);
  if (!mem || !bmp) {
    printf("[shot] 建不了离屏位图，放弃\n");
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    Gdiplus::GdiplusShutdown(token);
    LocalFree(argv);
    return 1;
  }
  HGDIOBJ old = SelectObject(mem, bmp);

  /* SRCCOPY | CAPTUREBLT：CAPTUREBLT 能把分层窗口（我们的覆盖层）一起抓进来 */
  const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
  printf("[shot] BitBlt=%d err=%lu\n", (int)ok, (unsigned long)GetLastError());

  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  /* ⚠ GDI+ 的所有对象必须在 GdiplusShutdown **之前**析构，
     否则关 GDI+ 时那些对象还活着，析构里再访问已销毁的 GDI+ 内部状态 → 段错误。
     所以这里把 Bitmap 关进一个作用域。 */
  {
    Gdiplus::Bitmap gb(bmp, nullptr);
    CLSID png;
    if (!findPngEncoder(&png)) {
      printf("[shot] 找不到 PNG 编码器\n");
    } else {
      const Gdiplus::Status st = gb.Save(out, &png, nullptr);
      if (st == Gdiplus::Ok) {
        wprintf(L"[shot] 已保存 %ls (%dx%d @ %d,%d)\n", out, w, h, x, y);
        rc = 0;
      } else {
        printf("[shot] 保存失败, status=%d\n", (int)st);
      }
    }
  }

  DeleteObject(bmp);
  Gdiplus::GdiplusShutdown(token);
  LocalFree(argv);
  return rc;
}
