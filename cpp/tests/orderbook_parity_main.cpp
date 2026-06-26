// apps/orderbook_parity_main.cpp — orderbook 数值与 Python 逐函数对齐。
// 黄金值由 .venv/bin/python 跑 pm_trader.orderbook 生成 (见 scratchpad/gen_golden.py)。
#include "pmm/models.hpp"
#include "pmm/orderbook.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ob = pmm::orderbook;

namespace {
int g_fail = 0;

void ApproxImpl(double got, double want, const char* what) {
    const double tol = 1e-6 + 1e-9 * std::abs(want);
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %-28s got=%.10g want=%.10g\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
void EqI(long got, long want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-28s got=%ld want=%ld\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}

pmm::OrderBook make_book() {
    pmm::OrderBook b;
    b.bids = {{0.41, 500}, {0.40, 1000}};
    b.asks = {{0.43, 300}, {0.44, 800}, {0.45, 2000}};
    return b;
}
}  // namespace

int main() {
    // ---- fees ----
    ApproxImpl(ob::calculate_fee(0, 0.42, 100), 0.0, "fee_0");
    ApproxImpl(ob::calculate_fee(150, 0.42, 100), 0.6299999999999999, "fee_150");
    ApproxImpl(ob::calculate_fee(150, 0.001, 0.01), 0.0001, "fee_min");
    ApproxImpl(ob::calculate_fee(200, 0.97, 50), 0.03000000000000003, "fee_ext");

    const pmm::OrderBook b = make_book();

    // ---- buy fill ----
    const pmm::FillResult fb = ob::simulate_buy_fill(b, 200.0, 150, "fok");
    ApproxImpl(fb.avg_price, 0.4334975369458128, "buy_avg");
    ApproxImpl(fb.total_shares, 461.3636363636364, "buy_shares");
    ApproxImpl(fb.total_cost, 200.0, "buy_cost");
    ApproxImpl(fb.fee, 1.3004926108374382, "buy_fee");
    ApproxImpl(fb.slippage_bps, 321.369927281257, "buy_slip");
    EqI(fb.levels_filled, 2, "buy_levels");
    EqI(fb.filled ? 1 : 0, 1, "buy_filled");

    const pmm::FillResult fb2 = ob::simulate_buy_fill(b, 1e9, 150, "fok");
    EqI(fb2.filled ? 1 : 0, 0, "buy_fok_reject_filled");
    ApproxImpl(fb2.total_shares, 0.0, "buy_fok_reject_shares");

    // ---- sell fill ----
    const pmm::FillResult fs = ob::simulate_sell_fill(b, 600.0, 150, "fok");
    ApproxImpl(fs.avg_price, 0.4083333333333333, "sell_avg");
    ApproxImpl(fs.total_shares, 600.0, "sell_shares");
    ApproxImpl(fs.total_cost, 245.0, "sell_cost");
    ApproxImpl(fs.fee, 3.675, "sell_fee");
    ApproxImpl(fs.slippage_bps, -277.7777777777776, "sell_slip");
    EqI(fs.levels_filled, 2, "sell_levels");

    // ---- maker math ----
    ApproxImpl(ob::maker_quote_score(100.0, 1.0, 3.0), 44.44444444444444, "mqs");
    ApproxImpl(ob::maker_reward_share(100.0, 1.0, 3.0, 250.0), 0.1509433962264151, "mrs");
    ApproxImpl(ob::reward_accrual(0.25, 500.0, 3600.0), 5.208333333333333, "acc");
    ApproxImpl(ob::adverse_bleed(100.0, 1.0, 0.50, 0.53, 0.2), 1.6000000000000023, "bleed");
    ApproxImpl(ob::skewed_center(0.50, 40.0, 100.0, 1.0, 0.5), 0.498, "skew");
    {
        const auto [dinv, loss] = ob::maker_fill(0.50, 0.515, 10.0, 100.0, 1.0, 0.5, 0.2, 200.0);
        ApproxImpl(dinv, -80.0, "mf_dinv");
        ApproxImpl(loss, 0.44000000000000483, "mf_loss");
    }
    ApproxImpl(ob::committed_capital(100.0, 1.0), 98.0, "cap");
    ApproxImpl(ob::book_inband_qmin(b, 0.42, 3.0), 222.22222222222183, "qmin");
    {
        const auto [better, at] = ob::depth_ahead(b, 0.41, "bid");
        ApproxImpl(better, 0.0, "da_better");
        ApproxImpl(at, 500.0, "da_at");
    }

    // ---- optimal quoting ----
    ApproxImpl(ob::expected_excess_move(2.0, 1.0), 0.7911862296052241, "eem");
    {
        const std::vector<ob::PricePoint> hist = {
            {0.50, 0}, {0.51, 60}, {0.49, 120}, {0.52, 180}, {0.50, 240}};
        ApproxImpl(ob::realized_sigma_c_from_history(hist, 1.0), 0.2738612787525833, "sigma");
    }
    {
        const ob::OptimalHalfSpread oh =
            ob::optimal_half_spread(500.0, 3.0, 100.0, 0.1, 250.0, 2.0, 86400.0, 0.2, 200);
        ApproxImpl(oh.half_spread_c, 3.0, "oh_half_spread_c");
        ApproxImpl(oh.net_per_day, -8102.7423, "oh_net_per_day");
        ApproxImpl(oh.reward_per_day, 0.0, "oh_reward_per_day");
        ApproxImpl(oh.bleed_per_day, 8102.7423, "oh_bleed_per_day");
        ApproxImpl(oh.share, 0.0, "oh_share");
        const ob::OptimalHalfSpread oh0 =
            ob::optimal_half_spread(500.0, 3.0, 100.0, 0.1, 250.0, 0.0, 86400.0, 0.0, 200);
        ApproxImpl(oh0.half_spread_c, 0.1, "oh0_hs (sigma=0 -> tick)");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
