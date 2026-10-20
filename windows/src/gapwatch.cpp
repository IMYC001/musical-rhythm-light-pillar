/* gapwatch.exe —— 纯调度间隙探针（完全不含 D3D / DXGI / 窗口）。
 *
 * 为什么需要它：
 *   原生版实测「每 4.86 秒一次、Present 阻塞约 400ms」的长帧，而且周期
 *   与帧率上限**无关**（30fps 与 120fps 下都是 4.86 秒）—— 这说明触发源
 *   是墙钟上的周期事件，不是交换链队列。
 *   要区分「是我们自己的代码/D3D 的锅」还是「这台机器上本来就有周期性
 *   停顿（沙箱、安全软件、CPU 配额节流）」，就得有一个**只做计时、
 *   什么都不干**的对照物：
 *     对照组 = 本程序（纯自旋计时）
 *     实验组 = 光柱.exe --perf
 *   如果对照组也在同样的周期上出现同样的长间隙，那长帧就不是我们的问题，
 *   再怎么调交换链参数也没用。
 *
 * 用法： gapwatch.exe <秒数>
 * 输出： 间隙 p50 / p95 / p99 / 最长，以及所有 > 25ms 的间隙出现的时刻
 */
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

static double nowMs() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char** argv) {
  const double secs = (argc > 1) ? atof(argv[1]) : 20.0;

  /* 自旋 + 每轮一次极短的等待，模拟真实主循环「干活 + 等下一帧」的形态。
     等待用高精度可等待定时器，和主程序完全一致 —— 这样两边的差异
     只剩下「有没有 D3D Present」这一个变量。 */
  HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,
      TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

  const double targetMs = 1000.0 / 60.0;
  std::vector<double> gaps;
  std::vector<double> hitchAt;
  double t = 0.0, last = nowMs(), t0 = last;
  volatile double sink = 0.0;

  while (t < secs) {
    const double now = nowMs();
    const double dt = now - last;
    if (dt < targetMs) {
      const double leftMs = targetMs - dt;
      if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(leftMs * 10000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
          WaitForSingleObject(timer, 50);
      } else {
        Sleep(1);
      }
      continue;
    }
    gaps.push_back(dt);
    if (dt > 25.0) hitchAt.push_back(t);
    /* 一点点真实计算，免得编译器把循环优化掉 */
    for (int i = 0; i < 2000; i++) sink += (double)i * 1e-9;
    last = now;
    t = (now - t0) / 1000.0;
  }

  std::vector<double> s = gaps;
  std::sort(s.begin(), s.end());
  auto q = [&](double p) {
    if (s.empty()) return 0.0;
    size_t i = (size_t)(p * (s.size() - 1) + 0.5);
    return s[i];
  };
  double sum = 0.0, mx = 0.0;
  for (double v : gaps) { sum += v; if (v > mx) mx = v; }

  printf("[gapwatch] 样本 %ld　均值 %.2f ms（%.1f fps）　p50 %.2f　p95 %.2f　p99 %.2f　最长 %.2f\n",
         (long)gaps.size(), gaps.empty() ? 0.0 : sum / gaps.size(),
         gaps.empty() ? 0.0 : 1000.0 / (sum / gaps.size()), q(0.50), q(0.95), q(0.99), mx);
  printf("[gapwatch] 长间隙（> 25 ms）%ld 个\n", (long)hitchAt.size());
  if (!hitchAt.empty()) {
    printf("[gapwatch] 长间隙出现时刻（秒）:");
    for (double v : hitchAt) printf(" %.2f", v);
    printf("\n");
  }
  if (timer) CloseHandle(timer);
  return 0;
}
