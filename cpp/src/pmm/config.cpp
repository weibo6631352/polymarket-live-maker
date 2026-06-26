// src/pmm/config.cpp — RunnerConfig::from_env / banner (port of runner.py)
#include "pmm/config.hpp"

#include <format>
#include <string>

#include "pmm/app/dotenv.hpp"
#include "pmm/env.hpp"

namespace pmm {

RunnerConfig RunnerConfig::from_env() {
    // 项目本地 .env -> 进程环境 (进程已有的不覆盖, 同 Python setdefault)。
    pmm::app::LoadDotEnv(".env", /*live_mode=*/false);

    using namespace pmm::env;
    RunnerConfig c;
    c.live = flag_eq("PM_TRADER_LIVE", "1", "0");
    c.dry_live = flag_ne("LM_DRY_LIVE", "0", "1");
    c.capital = f("LM_CAPITAL", 200.0);
    c.min_daily = f("LM_MIN_DAILY", 80.0);
    c.scan_top = i("LM_SCAN_TOP", 0);
    c.max_token_overlap = i("LM_MAX_TOKEN_OVERLAP", 1);
    c.poll_seconds = f("LM_POLL_SECONDS", 60.0);
    c.discovery_interval_s = f("LM_DISCOVERY_INTERVAL_S", 600.0);
    c.cooldown_rounds = i("LM_COOLDOWN_ROUNDS", 3);
    c.max_loss = f("LM_MAX_LOSS_PER_DAY", 20.0);
    c.retention_days = i("LM_RETENTION_DAYS", 30);
    c.events_enabled = flag_ne("LM_EVENTS", "0", "1");
    c.event_poll_every_s = f("LM_EVENT_POLL_EVERY_S", 10.0);
    c.stats_every_s = f("LM_STATS_EVERY_S", 60.0);
    c.max_req_per_sec = f("LM_MAX_REQ_PER_SEC", 149.0);
    c.write_reserve = f("LM_WRITE_RESERVE", 20.0);
    c.ws_enabled = flag_ne("LM_WS", "0", "1");
    c.resync_workers = i("LM_BOOK_RESYNC_WORKERS", 8);
    c.reeval_enabled = flag_ne("LM_REEVAL", "0", "1");
    c.reeval_interval_s = f("LM_REEVAL_INTERVAL_S", 300.0);
    c.min_pool_reward = f("LM_MIN_POOL_REWARD", 0.5);
    c.chop_aversion = f("LM_CHOP_AVERSION", 0.7);
    c.size_share_cap = f("LM_SIZE_SHARE_CAP", 0.33);
    c.quality_floor_frac = f("LM_QUALITY_FLOOR_FRAC", 0.10);
    c.max_pool_frac = f("LM_MAX_POOL_FRAC", 0.25);
    c.order_expiry_s = f("LM_ORDER_EXPIRY_S", 0.0);
    c.max_mid_vel_cps = f("LM_MAX_MID_VEL_CPS", 4.0);
    c.min_hold_s = f("LM_MIN_HOLD_S", 600.0);
    c.min_wallet_usdc = f("LM_MIN_WALLET_USDC", 0.0);
    c.use_optimal_spread = flag_eq("LM_OPTIMAL_SPREAD", "1", "0");
    c.recenter_ticks = i("LM_RECENTER_TICKS", 1);
    c.crossing_cost_c = f("LM_CROSSING_COST_C", 0.0);
    return c;
}

std::string RunnerConfig::banner() const {
    std::string mode;
    if (live) {
        mode = "LIVE — REAL MONEY";
    } else if (dry_live) {
        mode = "DRY-LIVE (live code path, no orders/fills -> P&L = gross reward only)";
    } else {
        mode = "PAPER (simulator)";
    }
    return std::format(
        "live-maker | mode={} | capital=${:.0f} | poll={:.0f}s | "
        "min_daily=${:.0f} | max_loss=${:.0f}",
        mode, capital, poll_seconds, min_daily, max_loss);
}

}  // namespace pmm
