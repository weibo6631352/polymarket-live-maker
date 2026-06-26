// apps/engine_smoke_main.cpp — engine↔db 记账粘合自检 (离线; 网络相关的 accrue/place 待 mock-api 集成测)。
#include "pmm/engine.hpp"
#include "pmm/orders.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pmm_engine_smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);

    pmm::Engine engine(dir);
    const pmm::Account acc = engine.init_account(1000.0);
    Check(acc.cash == 1000.0, "init_account cash=1000");

    // 直接在 db 建一个 maker quote (绕过需要网络的 place_maker_quote)，验证记账粘合。
    pmm::MakerQuoteInput q;
    q.market_slug = "will-x";
    q.market_condition_id = "0xc";
    q.outcome = "yes";
    q.token_id = "777";
    q.size = 100.0;
    q.half_spread_c = 1.0;
    q.max_spread_c = 3.0;
    q.min_size = 100.0;
    q.daily_rate = 500.0;
    q.tick = 0.01;
    q.cancel_efficiency = 0.0;
    q.committed_capital = 98.0;
    q.last_mid = 0.50;
    q.entry_mid = 0.50;
    q.last_accrued_at = "2026-06-26T00:00:00.000000+00:00";
    const pmm::MakerQuote mq = engine.db().create_maker_quote(q);

    // summary 反映该 active quote
    const auto sum = engine.get_maker_summary();
    Check(sum["active_quotes"].get<int>() == 1, "summary active_quotes=1");
    Check(near(sum["committed_capital"].get<double>(), 98.0), "summary committed_capital=98");

    // snapshot_equity = cash + committed_maker_capital (无持仓 → 不触网)
    const double eq = engine.snapshot_equity();
    Check(near(eq, 1098.0), "snapshot_equity = 1000 + 98 committed");

    // get_maker_quotes 列出 active
    Check(engine.get_maker_quotes().size() == 1, "get_maker_quotes size=1");

    // cancel_maker_quote 释放资本回 cash
    const auto cancelled = engine.cancel_maker_quote(mq.id);
    Check(cancelled.has_value() && (*cancelled)["status"] == "cancelled", "cancel_maker_quote");
    Check(near(engine.get_account().cash, 1098.0), "capital released to cash (1000+98)");
    Check(engine.get_maker_quotes().empty(), "no active quotes after cancel");
    Check(engine.get_maker_summary()["active_quotes"].get<int>() == 0, "summary active=0 after cancel");

    engine.close();
    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
