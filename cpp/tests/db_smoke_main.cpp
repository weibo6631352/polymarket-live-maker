// apps/db_smoke_main.cpp — db + models 自检: 建库 → 账户 → 交易 → 持仓 → 结算 → 权益曲线 → 缓存。
#include "pmm/db.hpp"
#include "pmm/models.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pmm_db_smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);

    pmm::Database db(dir);
    db.init_schema();

    // account
    const pmm::Account acc = db.init_account(10000.0);
    Check(acc.id == 1 && acc.cash == 10000.0, "init_account cash=10000");
    db.update_cash(9500.0);
    Check(db.get_account()->cash == 9500.0, "update_cash -> 9500");

    // trade
    pmm::TradeInput ti;
    ti.market_condition_id = "0xcond";
    ti.market_slug = "will-x-happen";
    ti.market_question = "Will X happen?";
    ti.outcome = "yes";
    ti.side = "buy";
    ti.order_type = "fok";
    ti.avg_price = 0.42;
    ti.amount_usd = 100.0;
    ti.shares = 238.0;
    ti.fee_rate_bps = 0;
    ti.fee = 0.0;
    ti.slippage = 1.5;
    ti.levels_filled = 2;
    ti.is_partial = false;
    const pmm::Trade t = db.insert_trade(ti);
    Check(t.id >= 1 && t.outcome == "yes" && t.avg_price == 0.42, "insert_trade round-trip");
    Check(db.get_trades(10).size() == 1, "get_trades size=1");

    // position upsert + resolve
    pmm::PositionInput pi;
    pi.market_condition_id = "0xcond";
    pi.market_slug = "will-x-happen";
    pi.market_question = "Will X happen?";
    pi.outcome = "yes";
    pi.shares = 238.0;
    pi.avg_entry_price = 0.42;
    pi.total_cost = 100.0;
    const pmm::Position p = db.upsert_position(pi);
    Check(p.shares == 238.0 && p.total_cost == 100.0, "upsert_position");
    Check(db.get_open_positions().size() == 1, "get_open_positions size=1");
    const pmm::Position r = db.resolve_position("0xcond", "yes", 238.0);  // YES wins -> $1/share
    Check(r.is_resolved && r.shares == 0.0 && r.resolved_at.has_value(), "resolve_position");
    // realized = 0 + 238 - 100 = 138
    Check(r.realized_pnl > 137.999 && r.realized_pnl < 138.001, "realized_pnl == 138");
    Check(db.get_open_positions().empty(), "no open positions after resolve");

    // equity curve
    db.record_equity(10100.0);
    db.record_equity(10200.0);
    const auto curve = db.get_equity_curve();
    Check(curve.size() == 2 && curve[0] == 10100.0 && curve[1] == 10200.0, "equity_curve chronological");

    // cache (raw JSON text)
    db.set_cache("mkt:0xcond", R"({"slug":"will-x-happen"})");
    const auto cached = db.get_cache("mkt:0xcond");
    Check(cached.has_value() && cached->find("will-x-happen") != std::string::npos, "cache set/get");
    Check(!db.get_cache("missing").has_value(), "cache miss -> nullopt");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
