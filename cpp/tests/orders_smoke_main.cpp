// apps/orders_smoke_main.cpp — limit orders + maker_quotes CRUD 自检 (port of orders.py)。
#include "pmm/db.hpp"
#include "pmm/orders.hpp"

#include <cstdio>
#include <filesystem>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pmm_orders_smoke";
    std::error_code ec;
    fs::remove_all(dir, ec);

    pmm::Database db(dir);
    db.init_schema();
    db.init_orders_schema();

    // ---- limit orders ----
    pmm::LimitOrderInput in;
    in.market_slug = "will-x";
    in.market_condition_id = "0xc";
    in.outcome = "yes";
    in.side = "buy";
    in.amount = 100.0;
    in.limit_price = 0.40;
    in.order_type = "gtc";
    const pmm::LimitOrder o = db.create_order(in);
    Check(o.id >= 1 && o.status == "pending" && !o.expires_at.has_value(), "create gtc order");
    Check(db.get_pending_orders().size() == 1, "1 pending order");
    Check(pmm::should_fill(o, 0.39) && !pmm::should_fill(o, 0.41), "should_fill buy logic");

    const auto cancelled = db.cancel_order(o.id);
    Check(cancelled && cancelled->status == "cancelled", "cancel order");
    Check(db.get_pending_orders().empty(), "0 pending after cancel");

    // gtd expiry (past timestamp → expires immediately)
    pmm::LimitOrderInput g;
    g.market_slug = "y";
    g.market_condition_id = "0xd";
    g.outcome = "no";
    g.side = "sell";
    g.amount = 50.0;
    g.limit_price = 0.60;
    g.order_type = "gtd";
    g.expires_at = "2000-01-01T00:00:00+00:00";  // long past
    const pmm::LimitOrder go = db.create_order(g);
    Check(go.order_type == "gtd" && go.expires_at.has_value(), "create gtd order");
    const auto expired = db.expire_orders();
    Check(expired.size() == 1 && db.get_order(go.id)->status == "expired", "expire_orders past-due gtd");

    // ---- maker quotes ----
    pmm::MakerQuoteInput q;
    q.market_slug = "will-x";
    q.market_condition_id = "0xc";
    q.outcome = "yes";
    q.token_id = "12345";
    q.size = 100.0;
    q.half_spread_c = 1.0;
    q.max_spread_c = 3.0;
    q.min_size = 100.0;
    q.daily_rate = 500.0;
    q.tick = 0.01;
    q.cancel_efficiency = 0.2;
    q.committed_capital = 98.0;
    q.last_mid = 0.50;
    q.last_accrued_at = "2026-06-26T00:00:00+00:00";
    q.entry_mid = 0.50;
    const pmm::MakerQuote mq = db.create_maker_quote(q);
    Check(mq.id >= 1 && mq.status == "active" && mq.size == 100.0 && mq.daily_rate == 500.0,
          "create maker quote");
    Check(db.get_active_maker_quotes().size() == 1, "1 active maker quote");

    pmm::AccrualUpdate u;
    u.accrued_rewards = 1.25;
    u.realized_bleed = 0.3;
    u.fills = 2;
    u.last_mid = 0.51;
    u.last_accrued_at = "2026-06-26T00:01:00+00:00";
    u.inventory = -100.0;
    u.inventory_pnl = -0.44;
    const pmm::MakerQuote up = db.update_maker_quote_accrual(mq.id, u);
    Check(up.accrued_rewards == 1.25 && up.fills == 2 && up.inventory == -100.0 &&
              up.inventory_pnl == -0.44 && up.last_mid == 0.51,
          "update_maker_quote_accrual");

    const auto cq = db.cancel_maker_quote(mq.id);
    Check(cq && cq->status == "cancelled", "cancel maker quote");
    Check(db.get_active_maker_quotes().empty() && db.get_all_maker_quotes().size() == 1,
          "0 active, 1 total after cancel");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
