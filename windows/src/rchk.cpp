/* rchk.exe —— 环境体检：是不是远程会话、显示模式、DPI 缩放。
 * 目的：那个「每 ~4.86 秒一次、约 400ms 的 Present 阻塞」在应用侧
 * 无论怎么调交换链参数都消不掉，需要确认是不是「远程会话 / 虚拟显示」
 * 这类环境因素。 */
#include <windows.h>
#include <cstdio>

int main() {
  printf("SM_REMOTESESSION = %d\n", (int)GetSystemMetrics(SM_REMOTESESSION));
  printf("SM_CMONITORS     = %d\n", (int)GetSystemMetrics(SM_CMONITORS));
  printf("screen           = %dx%d\n",
         (int)GetSystemMetrics(SM_CXSCREEN), (int)GetSystemMetrics(SM_CYSCREEN));
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
    printf("mode             = %lux%lu @ %luHz\n",
           (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
           (unsigned long)dm.dmDisplayFrequency);
  HDC dc = GetDC(nullptr);
  printf("bpp=%d planes=%d\n", GetDeviceCaps(dc, BITSPIXEL), GetDeviceCaps(dc, PLANES));
  ReleaseDC(nullptr, dc);
  return 0;
}
