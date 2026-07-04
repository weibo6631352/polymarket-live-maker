// src/pmm/config.cpp — RunnerConfig::from_env / banner (port of runner.py)
#include "pmm/config.hpp"

#include <format>
#include <sstream>
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
    // 优化后默认 (顾问): 小账户去小而无人争的池 (不是大而拥挤的 $80 池)。
    c.min_daily = f("LM_MIN_DAILY", 15.0);
    c.half_spread_ticks = i("LM_HALF_SPREAD_TICKS", 1);
    c.risk_tolerance_days = f("LM_RISK_TOLERANCE_DAYS", 5.0);
    c.scan_top = i("LM_SCAN_TOP", 0);
    c.max_token_overlap = i("LM_MAX_TOKEN_OVERLAP", 1);
    // 主循环现在是事件驱动 (盘口移动/WS reflex 立刻唤醒重挂, 实时响应), poll_seconds 只作兜底心跳
    // (定期查成交/累计奖励, 防安静市场漏掉)。1s 心跳足够; 真正的反应靠事件唤醒, 不靠这个定时器。
    c.poll_seconds = f("LM_POLL_SECONDS", 1.0);
    c.discovery_interval_s = f("LM_DISCOVERY_INTERVAL_S", 600.0);
    c.cooldown_rounds = i("LM_COOLDOWN_ROUNDS", 3);
    c.max_loss = f("LM_MAX_LOSS_PER_DAY", 20.0);
    c.retention_days = i("LM_RETENTION_DAYS", 30);
    c.events_enabled = flag_ne("LM_EVENTS", "0", "1");
    c.event_poll_every_s = f("LM_EVENT_POLL_EVERY_S", 10.0);
    c.stats_every_s = f("LM_STATS_EVERY_S", 60.0);
    c.max_req_per_sec = f("LM_MAX_REQ_PER_SEC", 149.0);
    c.ws_enabled = flag_ne("LM_WS", "0", "1");
    c.resync_workers = i("LM_BOOK_RESYNC_WORKERS", 8);
    c.reeval_enabled = flag_ne("LM_REEVAL", "0", "1");
    c.reeval_interval_s = f("LM_REEVAL_INTERVAL_S", 300.0);
    // 真 $1/天发放门槛: 0.5 会选到发 $0 的池; 对封顶份额判 1.5 留余量。
    c.min_pool_reward = f("LM_MIN_POOL_REWARD", 1.5);
    c.chop_aversion = f("LM_CHOP_AVERSION", 1.5);  // 在线波动=逆选择, 多躲
    c.size_share_cap = f("LM_SIZE_SHARE_CAP", 0.30);
    c.quality_floor_frac = f("LM_QUALITY_FLOOR_FRAC", 0.10);
    c.max_pool_frac = f("LM_MAX_POOL_FRAC", 0.12);  // 份额对规模是凹的→分散胜过梭哈
    c.order_expiry_s = f("LM_ORDER_EXPIRY_S", 120.0);  // GTD 死人开关: 进程挂了挂单自动过期
    c.max_mid_vel_cps = f("LM_MAX_MID_VEL_CPS", 1.0);  // 入场护栏: 别挂进快速/逆向的盘
    c.min_hold_s = f("LM_MIN_HOLD_S", 300.0);
    c.min_wallet_usdc = f("LM_MIN_WALLET_USDC", 0.0);
    c.use_optimal_spread = flag_ne("LM_OPTIMAL_SPREAD", "0", "1");  // 默认开: 高波动自动放宽
    c.micro_center = flag_eq("LM_MICRO_CENTER", "1", "0");  // 默认关: 47K 信号实测 β≈0 (预测层不支持)
    c.micro_gate_c = f("LM_MICRO_GATE_C", 0.3);
    c.micro_beta = f("LM_MICRO_BETA", 0.1);
    c.reward_calib = f("LM_REWARD_CALIB", 0.237);  // 利润校准 κ (真实/毛估; 实测 ~0.237)
    c.waterfill = flag_ne("LM_WATERFILL", "0", "1");  // 注水配资 (默认开)
    c.max_competitiveness = f("LM_MAX_COMPETITIVENESS", 1.5);  // 硬剔除新闻/毒池 (重新启用)
    c.extreme_mid_margin = f("LM_EXTREME_MID_MARGIN", 0.2);  // 剔除近极端价池 (逆选/趋势源)
    c.jump_vol_weight = f("LM_JUMP_VOL_WEIGHT", 0.7);  // 跳变感知 bleed (σ 下限=w×最大单步移动)
    c.net_edge_gate = flag_ne("LM_NET_EDGE_GATE", "0", "1");  // 净边际 ≤ 0 的池不报价 (默认开)
    c.orphan_sweep = flag_ne("LM_ORPHAN_SWEEP", "0", "1");    // 共享账户多策略时置 0 (见 config.hpp)
    c.tail_budget = f("LM_TAIL_BUDGET", 10.0);          // 尾部-VaR 预算 ($/单次成交最坏损失)
    c.tail_k_sigma = f("LM_TAIL_K_SIGMA", 3.0);         // 最坏跳变 = k×σ
    // R2(数学家): mid 仅 0.5¢ 偏移 share 即掉 ~36%, 且 mid 单调漂(重挂会"粘住"不抖) → 早重挂找回份额。
    c.recenter_ticks = i("LM_RECENTER_TICKS", 1);
    c.fade_micro_c = f("LM_FADE_MICRO_C", 1.0);  // fade-on-imbalance: micro-price 偏离阈 (¢)
    c.fade_obi = f("LM_FADE_OBI", 0.7);          // fade-on-imbalance: 订单流失衡阈 (0-1)
    c.fade_cooldown_s = f("LM_FADE_COOLDOWN_S", 10.0);  // fade 后冷却秒 (防紧抖动)
    c.fade_toxic_streak = i("LM_FADE_TOXIC_STREAK", 4);  // 连续 N poll 狂 fade → 退毒池
    {  // 静态白名单种子 (逗号分隔 condition_id; 空=不限)。主路径是外部 curator 经 DDS CuratorCommand 热推。
        std::stringstream ss(pmm::env::str("LM_POOL_WHITELIST"));
        std::string id;
        while (std::getline(ss, id, ',')) {
            const auto a = id.find_first_not_of(" \t");
            const auto b = id.find_last_not_of(" \t");
            if (a != std::string::npos) c.pool_whitelist.insert(id.substr(a, b - a + 1));
        }
    }
    c.min_days_to_resolution = f("LM_MIN_DAYS_TO_RESOLUTION", 10.0);  // R2: 剔除近结算催化剂池
    c.max_vol_mult = f("LM_MAX_VOL_MULT", 2.5);  // 剔除 实现日波动 > N×带宽 的跳池
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
