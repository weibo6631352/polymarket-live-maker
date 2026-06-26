// pmm/analytics.hpp — 绩效分析纯函数 (port of pm_trader/analytics.py)
//
// 从交易历史 + 账户算指标 (Sharpe / max-drawdown / win-rate / ROI / maker P&L 分离)。无副作用。
#pragma once

#include <vector>

#include "pmm/models.hpp"

namespace pmm::analytics {

struct Stats {
    double starting_balance{0.0};
    double cash{0.0};
    double positions_value{0.0};
    double committed_capital{0.0};
    double total_value{0.0};
    double pnl{0.0};
    double trading_pnl{0.0};
    double reward_income{0.0};
    double inventory_pnl{0.0};
    double net_maker_pnl{0.0};
    double roi_pct{0.0};
    int total_trades{0};
    int buy_count{0};
    int sell_count{0};
    double win_rate{0.0};
    double sharpe_ratio{0.0};
    double max_drawdown{0.0};
    double total_fees{0.0};
    double avg_trade_size{0.0};
};

// compute_stats 的可选 kwargs。equity_curve 为空 = 未提供 (退回交易现金流代理)。
struct StatsParams {
    double positions_value{0.0};
    std::vector<double> equity_curve;
    double reward_income{0.0};
    double inventory_pnl{0.0};
    double committed_capital{0.0};
};

[[nodiscard]] Stats compute_stats(const std::vector<Trade>& trades, const Account& account,
                                  const StatsParams& p = {});

[[nodiscard]] double sharpe_ratio_from_equity(const std::vector<double>& equity_curve);
[[nodiscard]] double max_drawdown_from_equity(const std::vector<double>& equity_curve);
[[nodiscard]] double win_rate(const std::vector<Trade>& trades);
[[nodiscard]] double sharpe_ratio(const std::vector<Trade>& trades_chronological,
                                  double starting_balance, int annualize_days = 365);
[[nodiscard]] double max_drawdown(const std::vector<Trade>& trades_chronological,
                                  double starting_balance);

}  // namespace pmm::analytics
