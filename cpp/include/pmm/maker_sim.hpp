// pmm/maker_sim.hpp — 做市奖励纸面回测 (port of pm_trader/maker_sim.py)
//
// 直接模拟做市策略 (paper Engine 是 taker-only 不能给 resting 单计奖励): 两边 min_size 报价贴在 band 内
// 一跳处; 每步在 band 内赚池子日 USDC 的一份额; mid 穿过报价时陈旧侧被挑选 (逆向成交, 成本 ≈
// (|Δmid|-tick)*size); colocation 更快撤单避开一部分 = cancel_efficiency 杠杆。net = 奖励 - 出血。
// 核心 simulate_pool() 是纯函数 (可离线回测/单测); run_experiment() 用真实 CLOB 价格历史驱动。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/orderbook.hpp"  // PricePoint
#include "pmm/rewards.hpp"    // RewardsClient, RewardConfig

namespace pmm::maker_sim {

constexpr double SECONDS_PER_DAY = 86400.0;

struct SimParams {
    double daily{0.0};         // 池日奖励率 (USD)
    double share{0.0};         // 在 band 内时本 maker 的奖励份额 (0-1)
    double tick{0.0};          // 最小跳 (价格单位); 报价距 mid 一跳
    double min_size{0.0};      // 每侧报价 shares
    double dt_seconds{0.0};    // 每步墙钟秒 (定每步奖励)
    double cancel_efficiency{0.0};   // 快撤避开的逆向成交比例 (0=总被挑选, 1=从不) — colocation 杠杆
    double requote_downtime_s{0.0};  // 每次被挑选后单边持仓、重新对冲前的零奖励秒数
    double unwind_cost_ticks{0.0};   // 平掉被挑选留下的单边库存所付的跳数
};

struct SimResult {
    int steps{0};
    double reward_income{0.0};
    double adverse_bleed{0.0};
    double unwind_cost{0.0};
    double net{0.0};
    int pickoffs{0};
    double pickoff_rate{0.0};
    double capital{0.0};
    double net_annualized_pct{0.0};
    [[nodiscard]] nlohmann::json to_json() const;
};

// 在 midpoint 价格路径上模拟两边 min_size maker。纯函数。
[[nodiscard]] SimResult simulate_pool(const std::vector<double>& price_path, const SimParams& params);

// 估一个池当前对 min_size 一跳报价的奖励份额 (拉实时 book, 算绑定侧 Qmin)。book 空/单边/失败 → nullopt。
[[nodiscard]] std::optional<double> live_pool_share(rewards::RewardsClient& client,
                                                    const rewards::RewardConfig& pool);

struct ExperimentParams {
    double min_daily{rewards::MIN_DAILY};
    int top{20};
    double share{0.05};
    std::vector<double> cancel_efficiencies{0.0, 0.9};
    bool use_scanned_share{true};
    int fidelity{1440};
    double requote_downtime_s{0.0};
    double unwind_cost_ticks{0.0};
};

// 用真实 CLOB 价格历史对 top 池回测做市策略 (网络)。返回 per-pool + aggregate json。
[[nodiscard]] nlohmann::json run_experiment(rewards::RewardsClient& client, const ExperimentParams& params);

// 历史点间中位秒 (回退 3600)。
[[nodiscard]] double history_dt_seconds(const std::vector<orderbook::PricePoint>& history);
// 从历史抽出 midpoint 路径。
[[nodiscard]] std::vector<double> path_from_history(const std::vector<orderbook::PricePoint>& history);

}  // namespace pmm::maker_sim
