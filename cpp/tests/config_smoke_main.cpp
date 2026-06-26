// apps/config_smoke_main.cpp — RunnerConfig::from_env 自检: 默认值 + env 覆盖 + 标志位 + banner。
#include "pmm/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    // 不依赖 .env: 直接设进程环境 (LoadDotEnv 不覆盖已有环境)。
    ::setenv("PM_TRADER_LIVE", "1", 1);
    ::setenv("LM_CAPITAL", "1000", 1);
    ::setenv("LM_POLL_SECONDS", "1.5", 1);
    ::setenv("LM_WS", "0", 1);
    ::setenv("LM_QUALITY_FLOOR_FRAC", "0.25", 1);
    ::setenv("LM_RECENTER_TICKS", "2", 1);  // int(float("2"))

    const pmm::RunnerConfig c = pmm::RunnerConfig::from_env();

    Check(c.live, "PM_TRADER_LIVE=1 -> live");
    Check(c.capital == 1000.0, "LM_CAPITAL=1000");
    Check(c.poll_seconds == 1.5, "LM_POLL_SECONDS=1.5");
    Check(!c.ws_enabled, "LM_WS=0 -> ws off");
    Check(c.quality_floor_frac == 0.25, "LM_QUALITY_FLOOR_FRAC=0.25");
    Check(c.recenter_ticks == 2, "LM_RECENTER_TICKS=2");

    // 未设置的取默认值
    Check(c.min_daily == 80.0, "default min_daily=80");
    Check(c.max_req_per_sec == 149.0, "default max_req_per_sec=149");
    Check(c.cooldown_rounds == 3, "default cooldown_rounds=3");
    Check(c.dry_live, "default dry_live=true (LM_DRY_LIVE unset)");

    const std::string b = c.banner();
    Check(b.find("LIVE \xE2\x80\x94 REAL MONEY") != std::string::npos, "banner live mode");
    Check(b.find("capital=$1000") != std::string::npos, "banner capital=$1000");
    std::printf("banner: %s\n", b.c_str());

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
