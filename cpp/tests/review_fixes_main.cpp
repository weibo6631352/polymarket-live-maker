// apps/review_fixes_main.cpp — 验证 code-review 修复确实生效 (现有 parity 用干净数据, 不覆盖这些边界)。
#include "pmm/api.hpp"
#include "pmm/env.hpp"
#include "pmm/models.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/rewards.hpp"
#include "pmm/strutil.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    // #3 parse_rewards: 非数值 max_spread → 丢池 (Python try/except return None)
    const json bad = json::parse(R"({"condition_id":"0xr","minimum_tick_size":"0.01",
        "tokens":[{"token_id":"5"}],
        "rewards":{"rates":[{"rewards_daily_rate":100}],"max_spread":"abc","min_size":5}})");
    Check(!pmm::rewards::parse_rewards(bad).has_value(), "#3 non-numeric max_spread -> pool dropped");
    const json good = json::parse(R"({"condition_id":"0xr","minimum_tick_size":"0.01",
        "tokens":[{"token_id":"5"}],
        "rewards":{"rates":[{"rewards_daily_rate":100}],"max_spread":"3","min_size":5}})");
    Check(pmm::rewards::parse_rewards(good).has_value(), "#3 valid pool still parsed");

    // #4 utf8_prefix: 按码点截断, 结果是合法 UTF-8 (json.dump 不抛)
    std::string e5;
    for (int k = 0; k < 5; ++k) e5 += "\xc3\xa9";  // 'é' x5 = 10 bytes
    const std::string p3 = pmm::strutil::utf8_prefix(e5, 3);
    Check(p3.size() == 6, "#4 utf8_prefix(e_acute x5, 3) == 6 bytes (3 codepoints)");
    bool dumped = true;
    try {
        json j;
        j["q"] = p3;
        (void)j.dump();
    } catch (...) {
        dumped = false;
    }
    Check(dumped, "#4 truncated string is valid UTF-8 (json.dump ok)");

    // #5 env::f: 尾部垃圾 → 默认值; 干净值 → 解析
    ::setenv("PMM_REV_X", "1000oops", 1);
    Check(pmm::env::f("PMM_REV_X", 200.0) == 200.0, "#5 env::f trailing garbage -> default");
    ::setenv("PMM_REV_Y", "1500", 1);
    Check(pmm::env::f("PMM_REV_Y", 200.0) == 1500.0, "#5 env::f clean value -> parsed");

    // #6 find null-fallback: conditionId=null + condition_id 有值 → 不回退次键 (对齐 Python None)
    const json m = json::parse(R"({"conditionId":null,"condition_id":"0xAAA","slug":"s"})");
    const pmm::Market mk = pmm::parse_market(m);
    Check(mk.condition_id.empty(), "#6 conditionId=null -> no snake_case fallback (matches Python None)");

    // #7 利润校准 κ: 把竞争对手分充气 1/κ (实测 κ≈0.237, 毛估高估份额 4.2×) → 估计份额↓ → 奖励↓ →
    //    net=reward-bleed 的最优半宽变宽 (之前高估奖励→挂太紧→多被逆选)。验证这个行为方向。
    {
        const double daily = 50.0, v = 3.0, min_sz = 100.0, tick_c = 1.0, qmin = 200.0, sigma = 0.3;
        const double ppd = 86400.0;  // poll=1s
        const auto base = pmm::orderbook::optimal_half_spread(daily, v, min_sz, tick_c, qmin, sigma, ppd);
        const auto calib =
            pmm::orderbook::optimal_half_spread(daily, v, min_sz, tick_c, qmin / 0.237, sigma, ppd);
        Check(calib.share < base.share, "#7 kappa: inflated competition -> strictly lower estimated share");
        Check(calib.half_spread_c >= base.half_spread_c,
              "#7 kappa: lower reward -> wider-or-equal optimal half-spread");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
