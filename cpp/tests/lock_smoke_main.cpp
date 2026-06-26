// apps/lock_smoke_main.cpp — SingleInstanceLock 自检: 第二个实例被拒, 释放后可再获取。
#include "pmm/infra/process/single_instance.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using pmm::infra::process::SingleInstanceLock;
    using pmm::infra::process::SingleInstanceLockFailure;
    const fs::path dir = fs::temp_directory_path() / "pmm_lock_smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string pid = (dir / "live-maker.pid").string();

    // 第一个实例: 获取成功
    auto first = std::make_unique<SingleInstanceLock>(pid);
    Check(true, "first lock acquired");
    Check(fs::exists(pid), "pid file created");

    // 第二个实例 (同路径): 被拒 (~1s 重试后抛)
    bool refused = false;
    try {
        SingleInstanceLock second(pid);
    } catch (const SingleInstanceLockFailure& e) {
        refused = true;
        std::printf("    (expected refusal: %s)\n", e.what());
    }
    Check(refused, "second lock on same path refused");

    // 释放第一个 → 第三个可获取
    first.reset();
    bool reacquired = false;
    try {
        SingleInstanceLock third(pid);
        reacquired = true;
    } catch (const SingleInstanceLockFailure&) {
    }
    Check(reacquired, "lock re-acquirable after release");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
