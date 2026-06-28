// pmm/portfolio.hpp — 选池配资编排 (port of pm_trader/portfolio.py, 纯选择部分)
//
// 验证过的 edge 受容量约束: 每个两边 min_size 报价挣几刀/天但只锁 ~$50-100, 故策略是"把小账本铺到
// 多个不相关的 SAFE 中尾池"。select_pools 按风险调整收益在预算内挑出可投、分散的账本。
// (MakerPortfolio 包装 LiveMakerBot, 见 maker_live 之后。)
#pragma once

#include <set>
#include <string>
#include <vector>

#include "pmm/rewards.hpp"

namespace pmm::portfolio {

constexpr double DEFAULT_CAPITAL = 1000.0;

// 选中池的元信息 (select_pools 输出的一项)。
struct SelectedPool {
    std::string question;
    std::string condition_id;
    std::string token;
    double daily{0.0};
    double share{0.0};
    double min_size{0.0};
    double size{0.0};
    double tick{0.0};
    double max_spread_c{0.0};
    double half_spread_c{0.0};
    double committed_capital{0.0};
    double est_daily_reward{0.0};
    double risk_adj_score{0.0};
};

struct SelectParams {
    double capital{DEFAULT_CAPITAL};
    bool require_safe{true};
    int half_spread_ticks{1};
    double risk_tolerance_days{7.0};
    double chop_aversion{1.0};
    int max_token_overlap{1};
    double size_share_cap{0.33};
    double loss_budget{0.0};
    double quality_floor_frac{0.10};
    double max_pool_frac{0.25};
    bool waterfill{false};        // true: 注水配资 (边际 κ×奖励/$ 均衡); false: capital∝score (旧, parity)
    double reward_calib{1.0};     // 利润校准 κ (注水按真实份额; 竞争充气 1/κ)
    double max_competitiveness{0.0};  // 硬剔除 competitiveness > 此值的新闻/毒池; 0=关 (parity)
    double extreme_mid_margin{0.0};   // 剔除 mid<margin 或 >1-margin 的近极端价池 (逆选/趋势源); 0=关 (parity)
    std::set<std::string> cooldown;
};

// ---- 纯助手 (导出供单测) ----
[[nodiscard]] std::set<std::string> significant_tokens(const std::string& question);
[[nodiscard]] std::string cluster_key(const std::string& question);  // "" = 无匹配
[[nodiscard]] double risk_adjusted_score(const rewards::PoolReport& p, double risk_tolerance_days,
                                         double chop_aversion = 1.0);

// 按风险调整收益在预算内挑出可投、分散的账本。
[[nodiscard]] std::vector<SelectedPool> select_pools(const rewards::ScanResult& scan_report,
                                                     const SelectParams& params = {});

}  // namespace pmm::portfolio
