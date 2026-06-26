// apps/ratelimit_smoke_main.cpp — TokenBucket 自检: 容量/取用/reserve 保护/补充/计数。
#include "pmm/ratelimit.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    // rate=100/s, burst=10, reserve=3.
    pmm::TokenBucket tb(100.0, 10.0, 3.0);
    Check(std::abs(tb.available() - 10.0) < 0.05, "initial available ~= burst(10)");

    Check(tb.try_acquire(5.0), "high-pri take 5 ok");
    // 现在 ~5 个; 低优先级要 3 个 → floor=3 → 需要 >= 6 → 失败 (reserve 保护写)。
    Check(!tb.try_acquire(3.0, /*low_priority=*/true), "low-pri blocked by reserve");
    // 高优先级仍可取 (无 floor)。
    Check(tb.try_acquire(5.0), "high-pri take remaining 5 ok");
    Check(!tb.try_acquire(1.0, true), "low-pri blocked at ~0");

    // 补充: 睡 100ms @100/s → +10 → 封顶 10。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const double avail = tb.available();
    Check(avail > 9.0 && avail <= 10.0 + 1e-6, "refill back toward capacity");

    // acquire 阻塞直到拿到 (有充足 token, 立即返回)。
    Check(tb.acquire(2.0), "blocking acquire(2) ok");

    // 计数: 累计授予 = 5+5+2 = 12; 低优先级授予 = 0 (都被 reserve 挡了)。
    Check(std::abs(tb.granted() - 12.0) < 1e-9, "granted == 12");
    Check(std::abs(tb.granted_low() - 0.0) < 1e-9, "granted_low == 0");

    // timeout 路径: 要超过容量的量, 短超时 → 返回 false。
    pmm::TokenBucket tb2(1.0, 1.0, 0.0);  // 慢补充
    tb2.try_acquire(1.0);                  // 抽干
    const bool got = tb2.acquire(5.0, /*timeout=*/0.1);
    Check(!got, "acquire times out when starved");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
