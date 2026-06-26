// src/pmm/analytics.cpp — 绩效分析实现 (port of pm_trader/analytics.py)
#include "pmm/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pmm::analytics {

namespace {

// 按日期分组的净 P&L, 按日期序返回 (sorted(by_date.keys()))。
std::vector<double> daily_pnl(const std::vector<Trade>& chrono) {
    std::map<std::string, double> by_date;  // std::map 按 key 排序 = 日期序
    for (const auto& t : chrono) {
        const std::string date_str = t.created_at.substr(0, 10);
        if (t.side == "buy") {
            by_date[date_str] -= (t.amount_usd + t.fee);
        } else if (t.side == "sell") {
            by_date[date_str] += (t.amount_usd - t.fee);
        }
        // 非 buy/sell: 不创建键 (与 Python defaultdict 一致)。
    }
    std::vector<double> out;
    out.reserve(by_date.size());
    for (const auto& [k, v] : by_date) out.push_back(v);
    return out;
}

double avg_trade_size(const std::vector<Trade>& trades) {
    if (trades.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& t : trades) sum += t.amount_usd;
    return sum / static_cast<double>(trades.size());
}

}  // namespace

double sharpe_ratio_from_equity(const std::vector<double>& equity_curve) {
    if (equity_curve.size() < 2) return 0.0;
    std::vector<double> returns;
    for (std::size_t i = 1; i < equity_curve.size(); ++i) {
        const double prev = equity_curve[i - 1];
        const double cur = equity_curve[i];
        if (prev > 0.0) returns.push_back((cur - prev) / prev);
    }
    if (returns.size() < 2) return 0.0;
    double sum = 0.0;
    for (double r : returns) sum += r;
    const double mean_ret = sum / static_cast<double>(returns.size());
    double var_sum = 0.0;
    for (double r : returns) var_sum += (r - mean_ret) * (r - mean_ret);
    const double variance = var_sum / static_cast<double>(returns.size() - 1);
    const double std_ret = std::sqrt(variance);
    if (std_ret == 0.0) return 0.0;
    return mean_ret / std_ret;
}

double max_drawdown_from_equity(const std::vector<double>& equity_curve) {
    if (equity_curve.empty()) return 0.0;
    double peak = equity_curve[0];
    double max_dd = 0.0;
    for (double equity : equity_curve) {
        if (equity > peak) peak = equity;
        if (peak > 0.0) max_dd = std::max(max_dd, (peak - equity) / peak);
    }
    return max_dd;
}

double win_rate(const std::vector<Trade>& trades) {
    std::vector<const Trade*> sells;
    for (const auto& t : trades) {
        if (t.side == "sell") sells.push_back(&t);
    }
    if (sells.empty()) return 0.0;

    std::map<std::pair<std::string, std::string>, double> buy_cost;
    std::map<std::pair<std::string, std::string>, double> buy_shares;
    for (const auto& t : trades) {
        if (t.side == "buy") {
            const auto key = std::make_pair(t.market_condition_id, t.outcome);
            buy_cost[key] += t.amount_usd;
            buy_shares[key] += t.shares;
        }
    }

    int wins = 0;
    for (const Trade* t : sells) {
        const auto key = std::make_pair(t->market_condition_id, t->outcome);
        double total_shares = 0.0;
        auto it = buy_shares.find(key);
        if (it != buy_shares.end()) total_shares = it->second;
        double entry_price;
        if (total_shares > 0.0) {
            entry_price = buy_cost[key] / total_shares;
        } else {
            entry_price = t->avg_price;
        }
        if (t->avg_price > entry_price) ++wins;
    }
    return static_cast<double>(wins) / static_cast<double>(sells.size());
}

double sharpe_ratio(const std::vector<Trade>& trades_chronological, double starting_balance,
                    int annualize_days) {
    const std::vector<double> dpnl = daily_pnl(trades_chronological);
    if (dpnl.size() < 2) return 0.0;

    double cumulative = starting_balance;
    std::vector<double> daily_returns;
    daily_returns.reserve(dpnl.size());
    for (double pnl : dpnl) {
        daily_returns.push_back(cumulative > 0.0 ? pnl / cumulative : 0.0);
        cumulative += pnl;
    }

    double sum = 0.0;
    for (double r : daily_returns) sum += r;
    const double mean_ret = sum / static_cast<double>(daily_returns.size());
    double var_sum = 0.0;
    for (double r : daily_returns) var_sum += (r - mean_ret) * (r - mean_ret);
    const double variance = var_sum / static_cast<double>(daily_returns.size() - 1);
    const double std_ret = std::sqrt(variance);
    if (std_ret == 0.0) return 0.0;
    return (mean_ret / std_ret) * std::sqrt(static_cast<double>(annualize_days));
}

double max_drawdown(const std::vector<Trade>& trades_chronological, double starting_balance) {
    if (trades_chronological.empty()) return 0.0;
    double cumulative = starting_balance;
    double peak = cumulative;
    double max_dd = 0.0;
    for (const auto& t : trades_chronological) {
        if (t.side == "buy") {
            cumulative -= (t.amount_usd + t.fee);
        } else if (t.side == "sell") {
            cumulative += (t.amount_usd - t.fee);
        }
        if (cumulative > peak) peak = cumulative;
        if (peak > 0.0) max_dd = std::max(max_dd, (peak - cumulative) / peak);
    }
    return max_dd;
}

Stats compute_stats(const std::vector<Trade>& trades, const Account& account, const StatsParams& p) {
    Stats s;
    s.total_value = account.cash + p.positions_value + p.committed_capital;
    s.pnl = s.total_value - account.starting_balance;
    s.roi_pct = account.starting_balance != 0.0 ? (s.pnl / account.starting_balance * 100.0) : 0.0;

    s.net_maker_pnl = p.reward_income + p.inventory_pnl;
    s.trading_pnl = s.pnl - s.net_maker_pnl;

    // 逆序为时间序 (DB 给的是最新在前)。
    std::vector<Trade> chronological(trades.rbegin(), trades.rend());

    if (p.equity_curve.size() >= 2) {
        s.sharpe_ratio = sharpe_ratio_from_equity(p.equity_curve);
        s.max_drawdown = max_drawdown_from_equity(p.equity_curve);
    } else {
        s.sharpe_ratio = sharpe_ratio(chronological, account.starting_balance);
        s.max_drawdown = max_drawdown(chronological, account.starting_balance);
    }

    s.starting_balance = account.starting_balance;
    s.cash = account.cash;
    s.positions_value = p.positions_value;
    s.committed_capital = p.committed_capital;
    s.reward_income = p.reward_income;
    s.inventory_pnl = p.inventory_pnl;
    s.total_trades = static_cast<int>(trades.size());
    for (const auto& t : trades) {
        if (t.side == "buy") ++s.buy_count;
        if (t.side == "sell") ++s.sell_count;
        s.total_fees += t.fee;
    }
    s.win_rate = win_rate(trades);
    s.avg_trade_size = avg_trade_size(trades);
    return s;
}

}  // namespace pmm::analytics
