// apps/analytics_parity_main.cpp — analytics 数值与 Python 对齐 (golden 见 scratchpad/gen_analytics.py)。
#include "pmm/analytics.hpp"
#include "pmm/models.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace A = pmm::analytics;

namespace {
int g_fail = 0;
void Approx(double got, double want, const char* what) {
    const double tol = 1e-9 + 1e-9 * std::abs(want);
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %-22s got=%.12g want=%.12g\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
void EqI(long got, long want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-22s got=%ld want=%ld\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
pmm::Trade T(int id, std::string cond, std::string outcome, std::string side, double avg, double amt,
             double shares, double fee, std::string created) {
    pmm::Trade t;
    t.id = id;
    t.market_condition_id = std::move(cond);
    t.market_slug = "s";
    t.market_question = "q";
    t.outcome = std::move(outcome);
    t.side = std::move(side);
    t.order_type = "fok";
    t.avg_price = avg;
    t.amount_usd = amt;
    t.shares = shares;
    t.fee = fee;
    t.levels_filled = 1;
    t.created_at = std::move(created);
    return t;
}
}  // namespace

int main() {
    const std::vector<pmm::Trade> trades = {  // newest-first
        T(5, "0xA", "yes", "sell", 0.60, 120, 200, 0.5, "2026-06-03 10:00:00"),
        T(4, "0xB", "yes", "sell", 0.30, 30, 100, 0.2, "2026-06-02 09:00:00"),
        T(3, "0xA", "yes", "buy", 0.50, 100, 200, 0.4, "2026-06-02 08:00:00"),
        T(2, "0xB", "yes", "buy", 0.40, 40, 100, 0.3, "2026-06-01 12:00:00"),
        T(1, "0xA", "yes", "buy", 0.45, 90, 200, 0.35, "2026-06-01 11:00:00"),
    };
    pmm::Account acc;
    acc.id = 1;
    acc.starting_balance = 1000.0;
    acc.cash = 1080.0;
    const std::vector<double> eq = {1000.0, 1010.0, 990.0, 1025.0, 1080.0};

    Approx(A::win_rate(trades), 0.5, "win_rate");
    Approx(A::sharpe_ratio_from_equity(eq), 0.6207850352542462, "sr_eq");
    Approx(A::max_drawdown_from_equity(eq), 0.019801980198019802, "mdd_eq");
    std::vector<pmm::Trade> chrono(trades.rbegin(), trades.rend());
    Approx(A::sharpe_ratio(chrono, 1000.0), -2.650119391743435, "sr_tr");
    Approx(A::max_drawdown(chrono, 1000.0), 0.23104999999999995, "mdd_tr");

    A::StatsParams p;
    p.positions_value = 50.0;
    p.equity_curve = eq;
    p.reward_income = 12.0;
    p.inventory_pnl = -3.0;
    p.committed_capital = 20.0;
    const A::Stats s = A::compute_stats(trades, acc, p);
    Approx(s.total_value, 1150.0, "st_total_value");
    Approx(s.pnl, 150.0, "st_pnl");
    Approx(s.trading_pnl, 141.0, "st_trading_pnl");
    Approx(s.net_maker_pnl, 9.0, "st_net_maker_pnl");
    Approx(s.roi_pct, 15.0, "st_roi_pct");
    EqI(s.total_trades, 5, "st_total_trades");
    EqI(s.buy_count, 3, "st_buy_count");
    EqI(s.sell_count, 2, "st_sell_count");
    Approx(s.win_rate, 0.5, "st_win_rate");
    Approx(s.sharpe_ratio, 0.6207850352542462, "st_sharpe");
    Approx(s.max_drawdown, 0.019801980198019802, "st_max_drawdown");
    Approx(s.total_fees, 1.75, "st_total_fees");
    Approx(s.avg_trade_size, 76.0, "st_avg_trade_size");

    A::StatsParams p2;
    p2.positions_value = 50.0;
    const A::Stats s2 = A::compute_stats(trades, acc, p2);  // no equity curve -> trade-based
    Approx(s2.sharpe_ratio, -2.650119391743435, "st2_sharpe");
    Approx(s2.max_drawdown, 0.23104999999999995, "st2_mdd");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
