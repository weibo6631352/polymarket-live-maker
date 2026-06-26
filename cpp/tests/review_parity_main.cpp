// apps/review_parity_main.cpp — review.summarize 数值/排序 parity (golden 来自 pm_trader.review)。离线。
#include "pmm/review.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

pmm::MakerQuote mq(const std::string& cond, const std::string& slug, const std::string& status,
                   double acc, double bleed, double inv, int fills) {
    pmm::MakerQuote q;
    q.market_condition_id = cond;
    q.market_slug = slug;
    q.status = status;
    q.accrued_rewards = acc;
    q.realized_bleed = bleed;
    q.inventory_pnl = inv;
    q.fills = fills;
    return q;
}
}  // namespace

int main() {
    using nlohmann::json;
    const std::vector<pmm::MakerQuote> quotes = {
        mq("0xA", "mkt-a", "active", 10.0, 2.0, -1.0, 3),
        mq("0xA", "mkt-a", "cancelled", 5.0, 1.0, 0.5, 2),
        mq("0xB", "mkt-b", "active", 20.0, 0.0, 3.0, 1),
    };
    const std::vector<json> events = {
        {{"kind", "place"}},
        {{"kind", "place"}},
        {{"kind", "exit"}, {"reason", "daily_cut"}},
        {{"kind", "exit"}, {"reason", "daily_cut"}},
        {{"kind", "exit"}, {"reason", "jump_risk_rose"}},
        {{"kind", "discovery"}, {"safe", 12}},
        {{"kind", "discovery"}, {"safe", 8}},
    };
    const std::vector<json> actual = {
        {{"condition_id", "0xA"}, {"usdc", 6.0}},
        {{"condition_id", "0xA"}, {"usdc", 2.0}},
        {{"condition_id", "0xC"}, {"usdc", 4.0}},
    };

    const json r = pmm::review::summarize(quotes, events, actual);

    Check(near(r["estimated_reward_total"], 35.0), "estimated_reward_total = 35.0");
    Check(near(r["actual_reward_total"], 12.0), "actual_reward_total = 12.0");
    Check(near(r["reconciliation_ratio"], 0.343), "reconciliation_ratio = 0.343");
    Check(near(r["bleed_total"], 3.0), "bleed_total = 3.0");
    Check(near(r["inventory_pnl_total"], 2.5), "inventory_pnl_total = 2.5");
    Check(near(r["net_estimated"], 37.5), "net_estimated = 37.5");
    Check(near(r["net_actual"], 14.5), "net_actual = 14.5");

    Check(r["decisions"]["places"] == 2, "decisions.places = 2");
    Check(r["decisions"]["exits_by_reason"]["daily_cut"] == 2, "exits daily_cut = 2");
    Check(r["decisions"]["exits_by_reason"]["jump_risk_rose"] == 1, "exits jump_risk_rose = 1");
    Check(r["discovery"]["scans"] == 2, "discovery.scans = 2");
    Check(near(r["discovery"]["avg_safe_pools"], 10.0), "discovery.avg_safe_pools = 10.0");

    // per_pool 排序 + 聚合: 0xA(est15,act8,n2,cancelled) > 0xB(est20,act0,n1) > 0xC(est0,act4,n0)
    const json& pp = r["per_pool"];
    Check(pp.size() == 3, "per_pool has 3 markets");
    Check(pp[0]["condition_id"] == "0xA" && near(pp[0]["est_reward"], 15.0) && near(pp[0]["actual_reward"], 8.0) &&
              pp[0]["n_quotes"] == 2 && pp[0]["status"] == "cancelled",
          "per_pool[0] = 0xA aggregated (est15 act8 n2 cancelled)");
    Check(pp[1]["condition_id"] == "0xB" && near(pp[1]["est_reward"], 20.0), "per_pool[1] = 0xB (est20)");
    Check(pp[2]["condition_id"] == "0xC" && near(pp[2]["actual_reward"], 4.0) && pp[2]["n_quotes"] == 0,
          "per_pool[2] = 0xC (actual-only)");

    // format_report 冒烟 (含 ratio < 0.6 警告)
    const std::string txt = pmm::review::format_report(r);
    Check(txt.find("MAKER REVIEW") != std::string::npos && txt.find("actual/est = 0.34x") != std::string::npos,
          "format_report renders header + ratio");
    Check(txt.find("over-estimated") != std::string::npos, "format_report shows over-estimate warning (ratio<0.6)");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
