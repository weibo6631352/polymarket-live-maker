// apps/api_rewards_parity_main.cpp — api 解析 + rewards 评分 与 Python 对齐 (offline, 纯函数)。
// golden 见 scratchpad/gen_api.py。
#include "pmm/api.hpp"
#include "pmm/models.hpp"
#include "pmm/rewards.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {
int g_fail = 0;
void Approx(double got, double want, const char* what) {
    const double tol = 1e-6 + 1e-9 * std::abs(want);
    const bool ok = std::abs(got - want) <= tol;
    std::printf("[%s] %-22s got=%.12g want=%.12g\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
void Eq(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-22s got=%s want=%s\n", ok ? "PASS" : "FAIL", what, got.c_str(), want.c_str());
    if (!ok) ++g_fail;
}
void EqB(bool got, bool want, const char* what) {
    const bool ok = got == want;
    std::printf("[%s] %-22s got=%d want=%d\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    namespace R = pmm::rewards;

    // ---- parse_market (Gamma, camelCase + JSON-string 字段) ----
    const json gamma = json::parse(R"({
      "conditionId":"0xcond1","slug":"will-x","question":"Will X happen?","description":"desc",
      "outcomes":"[\"Yes\", \"No\"]","outcomePrices":"[\"0.42\", \"0.58\"]",
      "clobTokenIds":"[\"111\", \"222\"]","active":true,"closed":false,
      "volume":"12345.6","liquidity":999.0,"endDateIso":"2026-12-31","orderPriceMinTickSize":"0.01"})");
    const pmm::Market gm = pmm::parse_market(gamma);
    Eq(gm.condition_id, "0xcond1", "gm_cid");
    Eq(gm.slug, "will-x", "gm_slug");
    Eq(gm.outcomes[0], "Yes", "gm_o0");
    Approx(gm.outcome_prices[0], 0.42, "gm_p0");
    Approx(gm.outcome_prices[1], 0.58, "gm_p1");
    Eq(gm.tokens[0].token_id, "111", "gm_t0id");
    Eq(gm.tokens[1].outcome, "No", "gm_t1out");
    EqB(gm.active, true, "gm_active");
    Approx(gm.volume, 12345.6, "gm_vol");
    Approx(gm.liquidity, 999.0, "gm_liq");
    Approx(gm.tick_size, 0.01, "gm_tick");
    Eq(gm.end_date, "2026-12-31", "gm_end");

    // ---- parse_clob_market ----
    const json clob = json::parse(R"({"condition_id":"0xcond2","market_slug":"clob-slug","question":"Q2",
      "tokens":[{"token_id":"333","outcome":"Yes"},{"token_id":"444","outcome":"No"}],
      "active":"true","closed":"false","minimum_tick_size":"0.001","end_date_iso":"2026-11-30"})");
    const pmm::Market cm = pmm::parse_clob_market(clob);
    Eq(cm.condition_id, "0xcond2", "cm_cid");
    Eq(cm.slug, "clob-slug", "cm_slug");
    Eq(cm.tokens[0].token_id, "333", "cm_t0id");
    EqB(cm.active, true, "cm_active");
    Approx(cm.tick_size, 0.001, "cm_tick");

    // ---- parse_order_book ----
    const json book = json::parse(R"({"bids":[{"price":"0.41","size":"500"},{"price":"0.40","size":"1000"}],
                                       "asks":[{"price":"0.43","size":"300"}]})");
    const pmm::OrderBook ob = pmm::parse_order_book(book);
    Approx(static_cast<double>(ob.bids.size()), 2, "ob_nb");
    Approx(static_cast<double>(ob.asks.size()), 1, "ob_na");
    Approx(ob.bids[0].price, 0.41, "ob_b0p");
    Approx(ob.bids[1].size, 1000.0, "ob_b1s");
    Approx(ob.asks[0].price, 0.43, "ob_a0p");

    // ---- parse_rewards ----
    const json rwm = json::parse(R"({"condition_id":"0xr","question":"Reward Q","minimum_tick_size":"0.01",
      "tokens":[{"token_id":"555","outcome":"Yes"},{"token_id":"556","outcome":"No"}],
      "rewards":{"rates":[{"rewards_daily_rate":"300"},{"rewards_daily_rate":50}],"max_spread":"3","min_size":"100"}})");
    const auto pr = R::parse_rewards(rwm);
    EqB(pr.has_value(), true, "pr_present");
    Approx(pr->daily, 350.0, "pr_daily");
    Approx(pr->max_spread, 3.0, "pr_ms");
    Approx(pr->min_size, 100.0, "pr_minsz");
    Approx(pr->tick, 0.01, "pr_tick");
    Eq(pr->token, "555", "pr_token");
    Eq(pr->question, "Reward Q", "pr_q");

    // ---- inband_score / reward_share ----
    const auto [ibs, ibn] = R::inband_score(book["bids"], 0.42, 3.0, true);
    Approx(ibs, 333.333333333334, "ib_score");
    Approx(ibn, 605.0, "ib_not");
    Approx(R::reward_share(100.0, 0.01, 3.0, 50.0), 0.47058823529411764, "rs");

    // ---- classify_jump_risk ----
    const std::vector<double> prices = {0.50, 0.51, 0.49, 0.52, 0.50, 0.55, 0.48, 0.51, 0.50, 0.53, 0.49};
    const R::JumpRisk jr = R::classify_jump_risk(prices, 5.0, 100.0);
    Eq(jr.verdict, "SAFE", "jr_verdict");
    Approx(jr.days, 11, "jr_days");
    Approx(jr.max_jump_c.value_or(-1), 7.0, "jr_maxc");
    Approx(jr.daily_vol_c.value_or(-1), 1.76, "jr_volc");
    Approx(jr.days_wiped.value_or(-1), 1.4, "jr_wiped");

    // ---- score_pool ----
    const json sbook = json::parse(R"({"bids":[{"price":"0.41","size":"500"},{"price":"0.40","size":"1000"}],
      "asks":[{"price":"0.43","size":"300"},{"price":"0.44","size":"800"}]})");
    std::vector<pmm::orderbook::PricePoint> hist;
    for (std::size_t i = 0; i < prices.size(); ++i)
        hist.push_back({prices[i], static_cast<double>(i) * 86400.0});
    const auto sp = R::score_pool(*pr, sbook, hist);
    EqB(sp.has_value(), true, "sp_present");
    Approx(sp->mid, 0.42, "sp_mid");
    Approx(sp->spread_c, 2.0, "sp_spread_c");
    Approx(sp->inband_notional, 1224, "sp_inband_notional");
    Approx(sp->share, 0.1667, "sp_share");
    Approx(sp->min_side_score, 222.2222, "sp_min_side_score");
    EqB(sp->empty_band, false, "sp_empty_band");
    Approx(sp->reward_per_day, 58.33, "sp_reward_per_day");
    Approx(sp->gross_ann_pct, 21292, "sp_gross_ann_pct");
    Eq(sp->jump_verdict, "SAFE", "sp_jump_verdict");
    Approx(sp->max_jump_c.value_or(-1), 7.0, "sp_max_jump_c");
    Approx(sp->days_wiped.value_or(-1), 0.1, "sp_days_wiped");
    Approx(sp->daily, 350.0, "sp_daily");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
