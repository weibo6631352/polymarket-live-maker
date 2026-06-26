// apps/maker_parity_main.cpp — maker_live 策略 + LiveMakerBot 状态机与 Python 对齐 (golden 见 gen_maker.py)。
#include "pmm/maker_live.hpp"
#include "pmm/models.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace M = pmm::maker;

namespace {
int g_fail = 0;
void Approx(double got, double want, const char* what) {
    const double tol = 1e-9 + 1e-9 * std::abs(want);
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %-18s got=%.10g want=%.10g\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
void EqI(long got, long want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-18s got=%ld want=%ld\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    // ---- compute_two_sided_quotes ----
    const auto q0 = M::compute_two_sided_quotes(0.50, 1.0, 100.0, 0.01, 3.0, 0.0);
    Approx(q0[0].price, 0.49, "q0_bid");
    Approx(q0[1].price, 0.51, "q0_ask");
    const auto q1 = M::compute_two_sided_quotes(0.515, 1.0, 100.0, 0.01, 3.0, -0.4);
    Approx(q1[0].price, 0.51, "q1_bid");
    Approx(q1[1].price, 0.53, "q1_ask");

    // ---- plan_requote / gtd_expiration ----
    EqI(M::plan_requote(0.50, 0.515, 1.0, 0.01) ? 1 : 0, 1, "rq_t");
    EqI(M::plan_requote(0.500, 0.505, 1.0, 0.01) ? 1 : 0, 0, "rq_f");
    EqI(static_cast<long>(M::gtd_expiration(300, 1000.0)), 1300, "gtd_300");
    EqI(static_cast<long>(M::gtd_expiration(5, 1000.0)), 1060, "gtd_5");

    // ---- LiveMakerBot step 序列 ----
    M::MakerBotConfig cfg;
    cfg.token_id = "t";
    cfg.max_spread_c = 3.0;
    cfg.min_size = 100.0;
    cfg.tick = 0.01;
    cfg.half_spread_c = 1.0;
    cfg.dry_run = true;
    M::LiveMakerBot bot(cfg);

    pmm::OrderBook book;
    book.bids = {{0.49, 1000}, {0.48, 500}};
    book.asks = {{0.51, 1000}, {0.52, 500}};

    struct Want {
        double inv;
        int halt;
        int rq;
        double skew;
        int nsub;
    };
    const std::vector<double> mids = {0.50, 0.515, 0.50, 0.55, 0.56};
    const std::vector<Want> want = {
        {0.0, 0, 1, 0.0, 2}, {-100.0, 0, 1, -0.4, 3}, {0.0, 0, 1, 0.0, 3},
        {-100.0, 1, 0, 0.0, 1}, {-100.0, 1, 0, 0.0, 0}};
    const std::vector<double> want_bid = {0.49, 0.51, 0.49};
    const std::vector<double> want_ask = {0.51, 0.53, 0.51};
    const std::vector<double> want_share = {0.0816, 0.6154, 0.0816};

    for (std::size_t i = 0; i < mids.size(); ++i) {
        const M::MakerPlan p = bot.step(book, mids[i]);
        const std::string tag = "S" + std::to_string(i);
        Approx(p.inventory, want[i].inv, (tag + "_inv").c_str());
        EqI(p.halted ? 1 : 0, want[i].halt, (tag + "_halt").c_str());
        EqI(p.requote ? 1 : 0, want[i].rq, (tag + "_rq").c_str());
        Approx(p.skew_ticks, want[i].skew, (tag + "_skew").c_str());
        EqI(static_cast<long>(p.submitted.size()), want[i].nsub, (tag + "_nsub").c_str());
        if (!p.halted) {
            Approx(p.orders[0].price, want_bid[i], (tag + "_bid").c_str());
            Approx(p.orders[1].price, want_ask[i], (tag + "_ask").c_str());
            Approx(p.est_reward_share, want_share[i], (tag + "_share").c_str());
        }
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
