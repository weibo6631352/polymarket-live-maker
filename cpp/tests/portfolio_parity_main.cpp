// apps/portfolio_parity_main.cpp — select_pools 选池配资与 Python 对齐 (golden 见 gen_portfolio.py)。
#include "pmm/portfolio.hpp"
#include "pmm/rewards.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace P = pmm::portfolio;
namespace R = pmm::rewards;

namespace {
int g_fail = 0;
void Approx(double got, double want, const char* what) {
    const double tol = 1e-4 + 1e-7 * std::abs(want);
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %-26s got=%.6g want=%.6g\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
void Eq(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-26s got=%s want=%s\n", ok ? "PASS" : "FAIL", what, got.c_str(), want.c_str());
    if (!ok) ++g_fail;
}

R::PoolReport pool(std::string q, std::string cid, std::string tok, double daily, double reward,
                   std::string verdict = "SAFE", double dwiped = 50.0, double volc = 1.0) {
    R::PoolReport p;
    p.question = std::move(q);
    p.condition_id = std::move(cid);
    p.token = std::move(tok);
    p.daily = daily;
    p.max_spread_c = 3.0;
    p.min_size = 100.0;
    p.tick = 0.01;
    p.share = 0.1;
    p.min_side_score = 200.0;
    p.empty_band = false;
    p.reward_per_day = reward;
    p.gross_ann_pct = 100.0;
    p.jump_verdict = std::move(verdict);
    p.max_jump_c = 5.0;
    p.daily_vol_c = volc;
    p.days_wiped = dwiped;
    return p;
}
}  // namespace

int main() {
    R::ScanResult report;
    report.pools = {
        pool("Will the Fed cut rates in December?", "0x1", "t1", 300, 30.0, "SAFE", 60),
        pool("FOMC March decision outcome", "0x2", "t2", 280, 28.0, "SAFE", 55),
        pool("Will Lakers championship parade happen", "0x3", "t3", 200, 18.0, "SAFE", 40),
        pool("Lakers championship odds this year", "0x4", "t4", 190, 17.0, "SAFE", 38),
        pool("Will it rain in Seattle tomorrow", "0x5", "t5", 150, 12.0, "SAFE", 30),
        pool("Bitcoin above 100k by July", "0x6", "t6", 120, 9.0, "SAFE", 25),
        pool("Tiny dust pool low score", "0x7", "t7", 90, 0.3, "SAFE", 5, 8.0),
        pool("Some KILL pool", "0x8", "t8", 400, 40.0, "KILL", 50),
    };

    // ---- 单一配资路径 (始终质量加权; loss_budget=0 / 预算紧时 size 退化到 min_size,
    //      但 share 始终按 size 重算 maker_reward_share) ----
    P::SelectParams params;  // capital=1000 默认; loss_budget=0 -> size 退化到 min_size=100
    const auto sel = P::select_pools(report, params);
    Approx(static_cast<double>(sel.size()), 4, "N");
    const std::vector<std::string> want_tok = {"t2", "t3", "t5", "t6"};
    const std::vector<double> want_ras = {2.371, 2.0106, 1.7027, 1.4766};
    const std::vector<double> want_edr = {50.9091, 36.3636, 27.2727, 21.8182};
    for (std::size_t i = 0; i < sel.size() && i < 4; ++i) {
        Eq(sel[i].token, want_tok[i], ("tok[" + std::to_string(i) + "]").c_str());
        Approx(sel[i].risk_adj_score, want_ras[i], ("ras[" + std::to_string(i) + "]").c_str());
        Approx(sel[i].size, 100.0, ("size[" + std::to_string(i) + "]").c_str());
        Approx(sel[i].committed_capital, 98.0, ("cap[" + std::to_string(i) + "]").c_str());
        Approx(sel[i].share, 0.1818, ("share[" + std::to_string(i) + "]").c_str());
        Approx(sel[i].est_daily_reward, want_edr[i], ("edr[" + std::to_string(i) + "]").c_str());
    }

    // loss_budget 抬高在此 fixture 下 size 仍受 min_size/份额上限约束, 结果不变 (确认单路径一致)。
    P::SelectParams dp;
    dp.loss_budget = 20.0;
    const auto sel2 = P::select_pools(report, dp);
    Approx(static_cast<double>(sel2.size()), 4, "lb N");
    for (std::size_t i = 0; i < sel2.size() && i < 4; ++i) {
        Approx(sel2[i].size, 100.0, ("lb size[" + std::to_string(i) + "]").c_str());
        Approx(sel2[i].est_daily_reward, want_edr[i], ("lb edr[" + std::to_string(i) + "]").c_str());
    }

    // ---- 注水配资 (waterfill): 按边际 κ×奖励/$ 注水, 取代退化到 min_size; 利用满预算、份额封顶、集中高边际 ----
    P::SelectParams wfp;
    wfp.waterfill = true;
    wfp.reward_calib = 0.237;
    const auto selw = P::select_pools(report, wfp);
    Approx(selw.empty() ? 0 : 1, 1, "wf selects pools");
    double wf_tot = 0.0, wf_maxsh = 0.0;
    for (const auto& s : selw) {
        wf_tot += s.committed_capital;
        wf_maxsh = std::max(wf_maxsh, s.share);
    }
    Approx(wf_tot > 4 * 98.0 ? 1 : 0, 1, "wf deploys > 4*min_size (utilizes budget, not min_size degrade)");
    Approx(wf_tot <= 1000.0 + 1.0 ? 1 : 0, 1, "wf total committed <= capital");
    Approx(wf_maxsh <= 0.34 ? 1 : 0, 1, "wf respects share cap (~0.33)");
    Approx(selw.front().committed_capital >= selw.back().committed_capital ? 1 : 0, 1,
           "wf concentrates capital in higher-marginal (top) pool");

    // ---- 竞争加权: PM market_competitiveness 惩罚 (挤池降权; 实测 0.46–4.2) ----
    {
        R::PoolReport quiet = pool("Quiet pool", "0xq1", "tq", 100, 10.0);
        R::PoolReport crowded = pool("Crowded pool", "0xc2", "tc", 100, 10.0);
        quiet.competitiveness = 0.47;   // 清静 (实测好选)
        crowded.competitiveness = 4.2;  // 极挤 (实测烂选)
        const double sq = P::risk_adjusted_score(quiet, 7.0, 1.0, 0.3);
        const double sc = P::risk_adjusted_score(crowded, 7.0, 1.0, 0.3);
        Approx(sc < sq ? 1 : 0, 1, "compet: crowded pool (comp 4.2) scores below quiet (comp 0.47), same reward");
        const double s0q = P::risk_adjusted_score(quiet, 7.0, 1.0, 0.0);
        const double s0c = P::risk_adjusted_score(crowded, 7.0, 1.0, 0.0);
        Approx(std::abs(s0q - s0c) < 1e-9 ? 1 : 0, 1, "compet: aversion=0 -> no penalty (parity preserved)");
    }

    // ---- 硬剔除拥挤池: max_competitiveness 上限 (churn源, 即使高奖励也剔除) ----
    {
        R::ScanResult rep2;
        R::PoolReport q1 = pool("Quiet alpha pool", "0xqa", "tqa", 100, 10.0);
        R::PoolReport c1 = pool("Crowded beta pool", "0xcb", "tcb", 500, 50.0);  // 高奖励但极挤
        q1.competitiveness = 0.47;
        c1.competitiveness = 4.2;
        rep2.pools = {q1, c1};
        P::SelectParams mp;
        mp.max_competitiveness = 1.5;
        const auto sm = P::select_pools(rep2, mp);
        bool hc = false, hq = false;
        for (const auto& s : sm) {
            if (s.token == "tcb") hc = true;
            if (s.token == "tqa") hq = true;
        }
        Approx(hc ? 0 : 1, 1, "max_competitiveness: crowded pool (comp 4.2, high reward) excluded");
        Approx(hq ? 1 : 0, 1, "max_competitiveness: quiet pool (comp 0.47) kept");
    }

    // ---- 助手 ----
    const auto toks = P::significant_tokens("Will the Fed cut rates in December?");
    const std::set<std::string> want_toks = {"cut", "december", "fed", "rates"};
    Approx(static_cast<double>(toks == want_toks ? 1 : 0), 1, "significant_tokens");
    Eq(P::cluster_key("FOMC March decision outcome"), "us-fed", "cluster_key fed");
    Eq(P::cluster_key("Will it rain in Seattle tomorrow"), "", "cluster_key none");
    Approx(P::risk_adjusted_score(report.pools[0], 7.0, 1.0), 2.350746, "risk_adjusted_score");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
