// 冒烟测试：验证工具链能不能出 Windows GUI exe、能不能调 Win32 API。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 能调到 user32 / dwmapi 就说明头文件与导入库都齐了
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    BOOL dwmOk = FALSE;
    DwmIsCompositionEnabled(&dwmOk);
    printf("[smoke] screen=%dx%d dwmComposition=%d\n", sw, sh, (int)dwmOk);
    fflush(stdout);
    return 0;
}
