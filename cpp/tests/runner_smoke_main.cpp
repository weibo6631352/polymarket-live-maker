// apps/runner_smoke_main.cpp — runner 离线安全逻辑自检 (degrade/kill-switch/cooldown/WS反射), 注入假依赖。
#include "pmm/engine.hpp"
#include "pmm/orders.hpp"
#include "pmm/runner.hpp"
#include "pmm/submitter.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

class FakeSubmitter : public pmm::ISubmitter {
public:
    std::vector<nlohmann::json> calls;
    nlohmann::json submit(const nlohmann::json& a) override {
        calls.push_back(a);
        nlohmann::json r = a;
        r["status"] = "OK";
        return r;
    }
    bool sent(const std::string& action, const std::string& token) const {
        for (const auto& c : calls)
            if (c.value("action", "") == action && c.value("token_id", "") == token) return true;
        return false;
    }
};

pmm::MakerQuote make_quote(pmm::Engine& e, const std::string& cond, const std::string& token,
                           double last_mid, double inv_pnl) {
    pmm::MakerQuoteInput q;
    q.market_slug = "s";
    q.market_condition_id = cond;
    q.outcome = "yes";
    q.token_id = token;
    q.size = 100;
    q.half_spread_c = 1.0;
    q.max_spread_c = 3.0;
    q.min_size = 100;
    q.daily_rate = 500;
    q.tick = 0.01;
    q.committed_capital = 98;
    q.last_mid = last_mid;
    q.entry_mid = last_mid;
    q.last_accrued_at = "2026-06-26T00:00:00.000000+00:00";
    const pmm::MakerQuote mq = e.db().create_maker_quote(q);
    if (inv_pnl != 0.0) {
        pmm::AccrualUpdate u;
        u.last_mid = last_mid;
        u.last_accrued_at = q.last_accrued_at;
        u.inventory_pnl = inv_pnl;
        e.db().update_maker_quote_accrual(mq.id, u);
    }
    return mq;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const fs::path base = fs::temp_directory_path() / "pmm_runner_smoke";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path state = base / "state";

    pmm::RunnerConfig cfg;
    cfg.state_dir = state.string();
    cfg.min_daily = 80.0;
    cfg.min_pool_reward = 0.5;
    cfg.max_loss = 20.0;
    cfg.recenter_ticks = 1;
    cfg.cooldown_rounds = 3;
    cfg.dry_live = true;
    cfg.live = false;
    cfg.ws_enabled = false;
    cfg.events_enabled = false;

    pmm::Engine engine(state);
    engine.init_account(1000.0);
    FakeSubmitter fake;
    pmm::LiveRunner runner(cfg, &engine, nullptr, &fake);

    // ---- degrade_reason ----
    Check(runner.degrade_reason(json{{"one_sided", true}}) == "one_sided", "degrade one_sided");
    Check(runner.degrade_reason(json{{"daily", 0.0}}) == "rewards_ended", "degrade rewards_ended");
    Check(runner.degrade_reason(json{{"daily", 50.0}}) == "daily_cut", "degrade daily_cut (<min_daily)");
    Check(runner.degrade_reason(json{{"daily", 100.0}, {"jump_verdict", "WATCH"}}) == "jump_risk_rose",
          "degrade jump_risk_rose");
    Check(runner.degrade_reason(json{{"daily", 100.0}, {"jump_verdict", "SAFE"}, {"empty_band", true}}) ==
              "empty_band",
          "degrade empty_band");
    Check(runner.degrade_reason(json{{"daily", 100.0}, {"jump_verdict", "SAFE"}, {"empty_band", false},
                                     {"reward_per_day", 0.2}}) == "reward_collapsed",
          "degrade reward_collapsed");
    Check(!runner.degrade_reason(json{{"daily", 100.0}, {"jump_verdict", "SAFE"}, {"empty_band", false},
                                      {"reward_per_day", 5.0}}).has_value(),
          "no degrade (healthy pool)");

    // ---- cooldown tick ----
    runner.cooldown()["x"] = 3;
    runner.tick_cooldowns();
    runner.tick_cooldowns();
    Check(runner.cooldown().count("x") == 1 && runner.cooldown()["x"] == 1, "cooldown 3->1 after 2 ticks");
    runner.tick_cooldowns();
    Check(runner.cooldown().count("x") == 0, "cooldown expires at 0");

    // ---- kill_check: healthy then max-loss then kill-file ----
    Check(!runner.kill_check().has_value(), "kill_check: healthy -> none");
    make_quote(engine, "0xloss", "Tloss", 0.5, /*inv_pnl=*/-25.0);  // inventory P&L -25 <= -max_loss 20
    {
        const auto k = runner.kill_check();
        Check(k.has_value() && k->rfind("max-loss", 0) == 0, "kill_check: inventory P&L <= -max_loss");
    }
    {
        std::ofstream(state / "KILL") << "x";  // KILL file (takes priority)
        const auto k = runner.kill_check();
        Check(k.has_value() && *k == "kill-file", "kill_check: KILL file");
        fs::remove(state / "KILL", ec);
    }

    // ---- WS reflex cancel ----
    make_quote(engine, "0xref", "Tref", 0.50, 0.0);
    runner.refresh_reflex_refs();
    runner.on_ws_price("Tref", 0.505);  // < band(0.01) -> no cancel
    Check(!fake.sent("CANCEL_ALL", "Tref"), "reflex: small move -> no cancel");
    runner.on_ws_price("Tref", 0.52);  // 0.02 >= 0.01 band -> CANCEL_ALL
    Check(fake.sent("CANCEL_ALL", "Tref"), "reflex: move beyond band -> CANCEL_ALL");

    engine.close();
    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
