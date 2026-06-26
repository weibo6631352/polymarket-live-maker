// apps/maker_sim_parity_main.cpp — simulate_pool 数值 parity (golden 来自 pm_trader.maker_sim)。离线。
#include "pmm/maker_sim.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int g_fail = 0;
void Near(double got, double want, const char* what, double tol = 1e-3) {
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %s: got=%.4f want=%.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    using namespace pmm::maker_sim;
    const std::vector<double> path = {0.50, 0.51, 0.50, 0.52, 0.49, 0.50, 0.505, 0.50, 0.50, 0.51, 0.50};

    // R1: eff=0 (总被挑选)
    SimParams p1;
    p1.daily = 500;
    p1.share = 0.05;
    p1.tick = 0.01;
    p1.min_size = 100;
    p1.dt_seconds = 600;
    p1.cancel_efficiency = 0.0;
    const SimResult r1 = simulate_pool(path, p1);
    std::printf("--- R1 (eff=0.0) ---\n");
    Near(r1.steps, 10, "steps");
    Near(r1.reward_income, 1.7361, "reward_income");
    Near(r1.adverse_bleed, 3.0, "adverse_bleed");
    Near(r1.unwind_cost, 0.0, "unwind_cost");
    Near(r1.net, -1.2639, "net");
    Near(r1.pickoffs, 7, "pickoffs");
    Near(r1.pickoff_rate, 0.7, "pickoff_rate");
    Near(r1.capital, 100, "capital");
    Near(r1.net_annualized_pct, -6643.0, "net_annualized_pct", 0.5);

    // R2: eff=0.9 + downtime 120s + unwind 1.5 ticks
    SimParams p2 = p1;
    p2.cancel_efficiency = 0.9;
    p2.requote_downtime_s = 120;
    p2.unwind_cost_ticks = 1.5;
    const SimResult r2 = simulate_pool(path, p2);
    std::printf("--- R2 (eff=0.9, downtime=120, unwind=1.5t) ---\n");
    Near(r2.reward_income, 1.4931, "reward_income");
    Near(r2.adverse_bleed, 0.3, "adverse_bleed");
    Near(r2.unwind_cost, 1.05, "unwind_cost");
    Near(r2.net, 0.1431, "net");
    Near(r2.net_annualized_pct, 751.9, "net_annualized_pct", 0.5);

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
