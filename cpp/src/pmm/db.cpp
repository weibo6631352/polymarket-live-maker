// src/pmm/db.cpp — SQLite 数据层实现 (port of pm_trader/db.py)
#include "pmm/db.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "pmm/models.hpp"
#include "pmm/orders.hpp"

namespace pmm {

namespace {

constexpr char kSchemaSql[] = R"SQL(
CREATE TABLE IF NOT EXISTS account (
    id INTEGER PRIMARY KEY DEFAULT 1,
    starting_balance REAL NOT NULL DEFAULT 10000,
    cash REAL NOT NULL DEFAULT 10000,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    CHECK (id = 1)
);

CREATE TABLE IF NOT EXISTS trades (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    market_condition_id TEXT NOT NULL,
    market_slug TEXT NOT NULL,
    market_question TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK (length(outcome) > 0),
    side TEXT NOT NULL CHECK (side IN ('buy', 'sell')),
    order_type TEXT NOT NULL DEFAULT 'fok' CHECK (order_type IN ('fok', 'fak')),
    avg_price REAL NOT NULL,
    amount_usd REAL NOT NULL,
    shares REAL NOT NULL,
    fee_rate_bps INTEGER NOT NULL,
    fee REAL NOT NULL DEFAULT 0,
    slippage REAL NOT NULL DEFAULT 0,
    levels_filled INTEGER NOT NULL DEFAULT 1,
    is_partial INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS positions (
    market_condition_id TEXT NOT NULL,
    market_slug TEXT NOT NULL,
    market_question TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK (length(outcome) > 0),
    shares REAL NOT NULL DEFAULT 0,
    avg_entry_price REAL NOT NULL DEFAULT 0,
    total_cost REAL NOT NULL DEFAULT 0,
    realized_pnl REAL NOT NULL DEFAULT 0,
    is_resolved INTEGER NOT NULL DEFAULT 0,
    resolved_at TEXT,
    PRIMARY KEY (market_condition_id, outcome)
);

CREATE TABLE IF NOT EXISTS market_cache (
    cache_key TEXT PRIMARY KEY,
    data TEXT NOT NULL,
    fetched_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS equity_curve (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    equity REAL NOT NULL,
    recorded_at TEXT NOT NULL DEFAULT (datetime('now'))
);
)SQL";

// equity_curve 每 poll append 一行, 热环从不读。无界则永久增长。保留滚动时间窗,
// 摊还 prune (非每次 insert) 以保持热路径单 INSERT。
constexpr int kEquityRetentionDays = 30;
constexpr long kEquityPruneEvery = 1000;

// ---- 轻量 RAII prepared-statement ----
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_text(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind_double(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    void bind_int(int i, int v) { sqlite3_bind_int(stmt_, i, v); }
    void bind_int64(int i, std::int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
    void bind_null(int i) { sqlite3_bind_null(stmt_, i); }
    void bind_opt_text(int i, const std::optional<std::string>& v) {
        if (v.has_value()) bind_text(i, *v);
        else bind_null(i);
    }

    // step: true = 有一行 (SQLITE_ROW), false = 完成 (SQLITE_DONE)。
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step failed: ") + sqlite3_errmsg(db_));
    }

    [[nodiscard]] int col_int(int i) const { return sqlite3_column_int(stmt_, i); }
    [[nodiscard]] double col_double(int i) const { return sqlite3_column_double(stmt_, i); }
    [[nodiscard]] std::string col_text(int i) const {
        const auto* p = sqlite3_column_text(stmt_, i);
        return p != nullptr ? reinterpret_cast<const char*>(p) : std::string{};
    }
    [[nodiscard]] bool col_is_null(int i) const {
        return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
    }
    [[nodiscard]] sqlite3* db() const noexcept { return db_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_{nullptr};
};

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err != nullptr ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg);
    }
}

Trade row_to_trade(const Stmt& s) {
    // 列序: id, market_condition_id, market_slug, market_question, outcome, side,
    //        order_type, avg_price, amount_usd, shares, fee_rate_bps, fee, slippage,
    //        levels_filled, is_partial, created_at  (SELECT * 顺序 = schema 顺序)
    Trade t;
    t.id = s.col_int(0);
    t.market_condition_id = s.col_text(1);
    t.market_slug = s.col_text(2);
    t.market_question = s.col_text(3);
    t.outcome = s.col_text(4);
    t.side = s.col_text(5);
    t.order_type = s.col_text(6);
    t.avg_price = s.col_double(7);
    t.amount_usd = s.col_double(8);
    t.shares = s.col_double(9);
    t.fee_rate_bps = s.col_int(10);
    t.fee = s.col_double(11);
    t.slippage = s.col_double(12);
    t.levels_filled = s.col_int(13);
    t.is_partial = s.col_int(14) != 0;
    t.created_at = s.col_text(15);
    return t;
}

Position row_to_position(const Stmt& s) {
    // 列序: market_condition_id, market_slug, market_question, outcome, shares,
    //        avg_entry_price, total_cost, realized_pnl, is_resolved, resolved_at
    Position p;
    p.market_condition_id = s.col_text(0);
    p.market_slug = s.col_text(1);
    p.market_question = s.col_text(2);
    p.outcome = s.col_text(3);
    p.shares = s.col_double(4);
    p.avg_entry_price = s.col_double(5);
    p.total_cost = s.col_double(6);
    p.realized_pnl = s.col_double(7);
    p.is_resolved = s.col_int(8) != 0;
    if (!s.col_is_null(9)) p.resolved_at = s.col_text(9);
    return p;
}

}  // namespace

Database::Database(std::filesystem::path data_dir)
    : data_dir_(std::move(data_dir)), db_path_(data_dir_ / "paper.db") {
    std::filesystem::create_directories(data_dir_);
}

Database::~Database() {
    close();
}

void Database::close() {
    if (conn_ != nullptr) {
        sqlite3_close(conn_);
        conn_ = nullptr;
    }
}

sqlite3* Database::conn() {
    if (conn_ == nullptr) {
        if (sqlite3_open(db_path_.c_str(), &conn_) != SQLITE_OK) {
            const std::string msg = conn_ != nullptr ? sqlite3_errmsg(conn_) : "open failed";
            throw std::runtime_error("sqlite open failed: " + msg);
        }
        exec(conn_, "PRAGMA journal_mode=WAL");
        exec(conn_, "PRAGMA synchronous=NORMAL");
        exec(conn_, "PRAGMA foreign_keys=ON");
    }
    return conn_;
}

void Database::init_schema() {
    exec(conn(), kSchemaSql);
}

bool Database::table_exists(const std::string& name) {
    Stmt s(conn(), "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?");
    s.bind_text(1, name);
    return s.step();
}

Account Database::init_account(double balance) {
    {
        Stmt s(conn(), "INSERT OR REPLACE INTO account (id, starting_balance, cash) VALUES (1, ?, ?)");
        s.bind_double(1, balance);
        s.bind_double(2, balance);
        s.step();
    }
    exec(conn(), "DELETE FROM trades");
    exec(conn(), "DELETE FROM positions");
    exec(conn(), "DELETE FROM equity_curve");
    if (table_exists("limit_orders")) exec(conn(), "DELETE FROM limit_orders");
    if (table_exists("maker_quotes")) exec(conn(), "DELETE FROM maker_quotes");
    auto acc = get_account();
    if (!acc) throw std::runtime_error("init_account: account missing after insert");
    return *acc;
}

std::optional<Account> Database::get_account() {
    Stmt s(conn(), "SELECT id, starting_balance, cash, created_at FROM account WHERE id = 1");
    if (!s.step()) return std::nullopt;
    Account a;
    a.id = s.col_int(0);
    a.starting_balance = s.col_double(1);
    a.cash = s.col_double(2);
    a.created_at = s.col_text(3);
    return a;
}

void Database::update_cash(double new_cash) {
    Stmt s(conn(), "UPDATE account SET cash = ? WHERE id = 1");
    s.bind_double(1, new_cash);
    s.step();
}

void Database::reset() {
    exec(conn(),
         "DROP TABLE IF EXISTS trades;"
         "DROP TABLE IF EXISTS positions;"
         "DROP TABLE IF EXISTS account;"
         "DROP TABLE IF EXISTS market_cache;"
         "DROP TABLE IF EXISTS limit_orders;"
         "DROP TABLE IF EXISTS maker_quotes;"
         "DROP TABLE IF EXISTS equity_curve;");
    init_schema();
}

void Database::record_equity(double equity) {
    {
        Stmt s(conn(), "INSERT INTO equity_curve (equity) VALUES (?)");
        s.bind_double(1, equity);
        s.step();
    }
    ++equity_inserts_;
    if (equity_inserts_ % kEquityPruneEvery == 0) {
        Stmt s(conn(), "DELETE FROM equity_curve WHERE recorded_at < datetime('now', ?)");
        s.bind_text(1, std::string("-") + std::to_string(kEquityRetentionDays) + " days");
        s.step();
    }
}

std::vector<double> Database::get_equity_curve() {
    Stmt s(conn(), "SELECT equity FROM equity_curve ORDER BY id");
    std::vector<double> out;
    while (s.step()) out.push_back(s.col_double(0));
    return out;
}

Trade Database::insert_trade(const TradeInput& t) {
    std::int64_t trade_id = 0;
    {
        Stmt s(conn(),
               "INSERT INTO trades ("
               "market_condition_id, market_slug, market_question,"
               "outcome, side, order_type,"
               "avg_price, amount_usd, shares,"
               "fee_rate_bps, fee, slippage,"
               "levels_filled, is_partial"
               ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        s.bind_text(1, t.market_condition_id);
        s.bind_text(2, t.market_slug);
        s.bind_text(3, t.market_question);
        s.bind_text(4, t.outcome);
        s.bind_text(5, t.side);
        s.bind_text(6, t.order_type);
        s.bind_double(7, t.avg_price);
        s.bind_double(8, t.amount_usd);
        s.bind_double(9, t.shares);
        s.bind_int(10, t.fee_rate_bps);
        s.bind_double(11, t.fee);
        s.bind_double(12, t.slippage);
        s.bind_int(13, t.levels_filled);
        s.bind_int(14, t.is_partial ? 1 : 0);
        s.step();
        trade_id = sqlite3_last_insert_rowid(conn());
    }
    Stmt s(conn(), "SELECT * FROM trades WHERE id = ?");
    s.bind_int64(1, trade_id);
    if (!s.step()) throw std::runtime_error("insert_trade: row missing after insert");
    return row_to_trade(s);
}

std::vector<Trade> Database::get_trades(int limit) {
    Stmt s(conn(), "SELECT * FROM trades ORDER BY id DESC LIMIT ?");
    s.bind_int(1, limit);
    std::vector<Trade> out;
    while (s.step()) out.push_back(row_to_trade(s));
    return out;
}

Position Database::upsert_position(const PositionInput& p) {
    {
        Stmt s(conn(),
               "INSERT INTO positions ("
               "market_condition_id, market_slug, market_question,"
               "outcome, shares, avg_entry_price, total_cost, realized_pnl"
               ") VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
               "ON CONFLICT (market_condition_id, outcome) DO UPDATE SET "
               "shares = excluded.shares,"
               "avg_entry_price = excluded.avg_entry_price,"
               "total_cost = excluded.total_cost,"
               "realized_pnl = excluded.realized_pnl");
        s.bind_text(1, p.market_condition_id);
        s.bind_text(2, p.market_slug);
        s.bind_text(3, p.market_question);
        s.bind_text(4, p.outcome);
        s.bind_double(5, p.shares);
        s.bind_double(6, p.avg_entry_price);
        s.bind_double(7, p.total_cost);
        s.bind_double(8, p.realized_pnl);
        s.step();
    }
    auto pos = get_position(p.market_condition_id, p.outcome);
    if (!pos) throw std::runtime_error("upsert_position: row missing after upsert");
    return *pos;
}

std::optional<Position> Database::get_position(const std::string& market_condition_id,
                                               const std::string& outcome) {
    Stmt s(conn(), "SELECT * FROM positions WHERE market_condition_id = ? AND outcome = ?");
    s.bind_text(1, market_condition_id);
    s.bind_text(2, outcome);
    if (!s.step()) return std::nullopt;
    return row_to_position(s);
}

std::vector<Position> Database::get_open_positions() {
    Stmt s(conn(), "SELECT * FROM positions WHERE is_resolved = 0 AND shares > 0");
    std::vector<Position> out;
    while (s.step()) out.push_back(row_to_position(s));
    return out;
}

std::vector<Position> Database::get_positions_for_market(const std::string& market_condition_id) {
    Stmt s(conn(), "SELECT * FROM positions WHERE market_condition_id = ?");
    s.bind_text(1, market_condition_id);
    std::vector<Position> out;
    while (s.step()) out.push_back(row_to_position(s));
    return out;
}

Position Database::resolve_position(const std::string& market_condition_id, const std::string& outcome,
                                    double payout) {
    auto position = get_position(market_condition_id, outcome);
    if (!position) {
        throw std::runtime_error("No position for " + market_condition_id + "/" + outcome);
    }
    const double new_realized = position->realized_pnl + payout - position->total_cost;
    {
        Stmt s(conn(),
               "UPDATE positions SET "
               "is_resolved = 1,"
               "resolved_at = datetime('now'),"
               "realized_pnl = ?,"
               "shares = 0 "
               "WHERE market_condition_id = ? AND outcome = ?");
        s.bind_double(1, new_realized);
        s.bind_text(2, market_condition_id);
        s.bind_text(3, outcome);
        s.step();
    }
    return *get_position(market_condition_id, outcome);
}

void Database::set_cache(const std::string& key, const std::string& json_text) {
    Stmt s(conn(),
           "INSERT OR REPLACE INTO market_cache (cache_key, data, fetched_at) "
           "VALUES (?, ?, datetime('now'))");
    s.bind_text(1, key);
    s.bind_text(2, json_text);
    s.step();
}

std::optional<std::string> Database::get_cache(const std::string& key) {
    Stmt s(conn(), "SELECT data FROM market_cache WHERE cache_key = ?");
    s.bind_text(1, key);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

std::optional<std::string> Database::get_cache_fresh(const std::string& key, int ttl_seconds) {
    // fetched_at 在最近 ttl_seconds 内 = 新鲜 (等价 Python age <= TTL); SQLite UTC 两边一致。
    // >= 对齐 Python `age <= TTL` 的包含边界 (秒粒度)。
    Stmt s(conn(), "SELECT data FROM market_cache WHERE cache_key = ? AND fetched_at >= datetime('now', ?)");
    s.bind_text(1, key);
    s.bind_text(2, "-" + std::to_string(ttl_seconds) + " seconds");
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

// ---------------------------------------------------------------------------
// limit orders + maker quotes (port of orders.py)
// ---------------------------------------------------------------------------

namespace {

constexpr char kOrdersSchema[] = R"SQL(
CREATE TABLE IF NOT EXISTS limit_orders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    market_slug TEXT NOT NULL,
    market_condition_id TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK (length(outcome) > 0),
    side TEXT NOT NULL CHECK (side IN ('buy', 'sell')),
    amount REAL NOT NULL,
    limit_price REAL NOT NULL,
    order_type TEXT NOT NULL CHECK (order_type IN ('gtc', 'gtd')),
    expires_at TEXT,
    status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'filled', 'cancelled', 'expired', 'rejected')),
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    filled_at TEXT
);

CREATE TABLE IF NOT EXISTS maker_quotes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    market_slug TEXT NOT NULL,
    market_condition_id TEXT NOT NULL,
    outcome TEXT NOT NULL CHECK (length(outcome) > 0),
    token_id TEXT NOT NULL,
    size REAL NOT NULL,
    half_spread_c REAL NOT NULL,
    max_spread_c REAL NOT NULL,
    min_size REAL NOT NULL,
    daily_rate REAL NOT NULL,
    tick REAL NOT NULL,
    cancel_efficiency REAL NOT NULL DEFAULT 0,
    max_inventory REAL NOT NULL DEFAULT 0,
    skew_strength REAL NOT NULL DEFAULT 0,
    inventory REAL NOT NULL DEFAULT 0,
    inventory_pnl REAL NOT NULL DEFAULT 0,
    entry_mid REAL NOT NULL DEFAULT 0,
    committed_capital REAL NOT NULL,
    accrued_rewards REAL NOT NULL DEFAULT 0,
    realized_bleed REAL NOT NULL DEFAULT 0,
    fills INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'cancelled')),
    last_mid REAL NOT NULL,
    created_at TEXT NOT NULL DEFAULT (datetime('now')),
    last_accrued_at TEXT NOT NULL,
    complement_token_id TEXT NOT NULL DEFAULT '',
    complement_inventory REAL NOT NULL DEFAULT 0
);
)SQL";

std::string utc_iso_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto us = duration_cast<microseconds>(now - secs).count();
    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long>(us));
    return buf;
}

LimitOrder row_to_order(const Stmt& s) {
    LimitOrder o;
    o.id = s.col_int(0);
    o.market_slug = s.col_text(1);
    o.market_condition_id = s.col_text(2);
    o.outcome = s.col_text(3);
    o.side = s.col_text(4);
    o.amount = s.col_double(5);
    o.limit_price = s.col_double(6);
    o.order_type = s.col_text(7);
    if (!s.col_is_null(8)) o.expires_at = s.col_text(8);
    o.status = s.col_text(9);
    o.created_at = s.col_text(10);
    if (!s.col_is_null(11)) o.filled_at = s.col_text(11);
    return o;
}

MakerQuote row_to_maker_quote(const Stmt& s) {
    MakerQuote q;
    q.id = s.col_int(0);
    q.market_slug = s.col_text(1);
    q.market_condition_id = s.col_text(2);
    q.outcome = s.col_text(3);
    q.token_id = s.col_text(4);
    q.size = s.col_double(5);
    q.half_spread_c = s.col_double(6);
    q.max_spread_c = s.col_double(7);
    q.min_size = s.col_double(8);
    q.daily_rate = s.col_double(9);
    q.tick = s.col_double(10);
    q.cancel_efficiency = s.col_double(11);
    q.max_inventory = s.col_double(12);
    q.skew_strength = s.col_double(13);
    q.inventory = s.col_double(14);
    q.inventory_pnl = s.col_double(15);
    q.entry_mid = s.col_double(16);
    q.committed_capital = s.col_double(17);
    q.accrued_rewards = s.col_double(18);
    q.realized_bleed = s.col_double(19);
    q.fills = s.col_int(20);
    q.status = s.col_text(21);
    q.last_mid = s.col_double(22);
    q.created_at = s.col_text(23);
    q.last_accrued_at = s.col_text(24);
    q.complement_token_id = s.col_is_null(25) ? std::string{} : s.col_text(25);
    q.complement_inventory = s.col_double(26);
    return q;
}

}  // namespace

void Database::init_orders_schema() {
    exec(conn(), kOrdersSchema);
    // 迁移: 旧 maker_quotes 表(无 complement_token_id 列)补列 — BUY-NO 双边做市。
    bool has_comp = false;
    {
        Stmt s(conn(),
               "SELECT 1 FROM pragma_table_info('maker_quotes') WHERE name = 'complement_token_id'");
        has_comp = s.step();
    }
    if (!has_comp) {
        exec(conn(), "ALTER TABLE maker_quotes ADD COLUMN complement_token_id TEXT NOT NULL DEFAULT ''");
    }
    // 迁移: 补 complement_inventory 列 — NO 腿 (BUY-NO) 持仓单独持久化, 跨轮重试平仓 (双边记账修复)。
    bool has_comp_inv = false;
    {
        Stmt s(conn(),
               "SELECT 1 FROM pragma_table_info('maker_quotes') WHERE name = 'complement_inventory'");
        has_comp_inv = s.step();
    }
    if (!has_comp_inv) {
        exec(conn(), "ALTER TABLE maker_quotes ADD COLUMN complement_inventory REAL NOT NULL DEFAULT 0");
    }
}

LimitOrder Database::create_order(const LimitOrderInput& o) {
    std::optional<std::string> expires;
    if (o.expires_at.has_value()) expires = normalize_timestamp(*o.expires_at);
    std::int64_t id = 0;
    {
        Stmt s(conn(),
               "INSERT INTO limit_orders (market_slug, market_condition_id, outcome, side, amount, "
               "limit_price, order_type, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        s.bind_text(1, o.market_slug);
        s.bind_text(2, o.market_condition_id);
        s.bind_text(3, o.outcome);
        s.bind_text(4, o.side);
        s.bind_double(5, o.amount);
        s.bind_double(6, o.limit_price);
        s.bind_text(7, o.order_type);
        s.bind_opt_text(8, expires);
        s.step();
        id = sqlite3_last_insert_rowid(conn());
    }
    return *get_order(static_cast<int>(id));
}

std::vector<LimitOrder> Database::get_pending_orders() {
    Stmt s(conn(), "SELECT * FROM limit_orders WHERE status = 'pending' ORDER BY id");
    std::vector<LimitOrder> out;
    while (s.step()) out.push_back(row_to_order(s));
    return out;
}

std::optional<LimitOrder> Database::get_order(int order_id) {
    Stmt s(conn(), "SELECT * FROM limit_orders WHERE id = ?");
    s.bind_int(1, order_id);
    if (!s.step()) return std::nullopt;
    return row_to_order(s);
}

std::optional<LimitOrder> Database::cancel_order(int order_id) {
    auto o = get_order(order_id);
    if (!o || o->status != "pending") return std::nullopt;
    {
        Stmt s(conn(), "UPDATE limit_orders SET status = 'cancelled' WHERE id = ?");
        s.bind_int(1, order_id);
        s.step();
    }
    return get_order(order_id);
}

std::vector<LimitOrder> Database::cancel_all_orders() {
    std::vector<LimitOrder> pending = get_pending_orders();
    if (pending.empty()) return {};
    exec(conn(), "UPDATE limit_orders SET status = 'cancelled' WHERE status = 'pending'");
    for (auto& o : pending) o.status = "cancelled";
    return pending;
}

LimitOrder Database::mark_filled(int order_id) {
    Stmt s(conn(), "UPDATE limit_orders SET status = 'filled', filled_at = datetime('now') WHERE id = ?");
    s.bind_int(1, order_id);
    s.step();
    return *get_order(order_id);
}

LimitOrder Database::reject_order(int order_id) {
    Stmt s(conn(), "UPDATE limit_orders SET status = 'rejected' WHERE id = ?");
    s.bind_int(1, order_id);
    s.step();
    return *get_order(order_id);
}

std::vector<LimitOrder> Database::expire_orders() {
    const std::string now = normalize_timestamp(utc_iso_now());
    std::vector<LimitOrder> out;
    {
        Stmt s(conn(),
               "SELECT * FROM limit_orders WHERE status = 'pending' AND order_type = 'gtd' AND "
               "expires_at <= ?");
        s.bind_text(1, now);
        while (s.step()) out.push_back(row_to_order(s));
    }
    if (!out.empty()) {
        Stmt s(conn(),
               "UPDATE limit_orders SET status = 'expired' WHERE status = 'pending' AND order_type "
               "= 'gtd' AND expires_at <= ?");
        s.bind_text(1, now);
        s.step();
    }
    return out;
}

MakerQuote Database::create_maker_quote(const MakerQuoteInput& q) {
    std::int64_t id = 0;
    {
        Stmt s(conn(),
               "INSERT INTO maker_quotes (market_slug, market_condition_id, outcome, token_id, size, "
               "half_spread_c, max_spread_c, min_size, daily_rate, tick, cancel_efficiency, "
               "max_inventory, skew_strength, entry_mid, committed_capital, last_mid, last_accrued_at, "
               "complement_token_id) "
               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        s.bind_text(1, q.market_slug);
        s.bind_text(2, q.market_condition_id);
        s.bind_text(3, q.outcome);
        s.bind_text(4, q.token_id);
        s.bind_double(5, q.size);
        s.bind_double(6, q.half_spread_c);
        s.bind_double(7, q.max_spread_c);
        s.bind_double(8, q.min_size);
        s.bind_double(9, q.daily_rate);
        s.bind_double(10, q.tick);
        s.bind_double(11, q.cancel_efficiency);
        s.bind_double(12, q.max_inventory);
        s.bind_double(13, q.skew_strength);
        s.bind_double(14, q.entry_mid);
        s.bind_double(15, q.committed_capital);
        s.bind_double(16, q.last_mid);
        s.bind_text(17, q.last_accrued_at);
        s.bind_text(18, q.complement_token_id);
        s.step();
        id = sqlite3_last_insert_rowid(conn());
    }
    return *get_maker_quote(static_cast<int>(id));
}

std::optional<MakerQuote> Database::get_maker_quote(int quote_id) {
    Stmt s(conn(), "SELECT * FROM maker_quotes WHERE id = ?");
    s.bind_int(1, quote_id);
    if (!s.step()) return std::nullopt;
    return row_to_maker_quote(s);
}

std::vector<MakerQuote> Database::get_active_maker_quotes() {
    Stmt s(conn(), "SELECT * FROM maker_quotes WHERE status = 'active' ORDER BY id");
    std::vector<MakerQuote> out;
    while (s.step()) out.push_back(row_to_maker_quote(s));
    return out;
}

std::vector<MakerQuote> Database::get_all_maker_quotes() {
    Stmt s(conn(), "SELECT * FROM maker_quotes ORDER BY id");
    std::vector<MakerQuote> out;
    while (s.step()) out.push_back(row_to_maker_quote(s));
    return out;
}

std::optional<MakerQuote> Database::cancel_maker_quote(int quote_id) {
    auto q = get_maker_quote(quote_id);
    if (!q || q->status != "active") return std::nullopt;
    {
        Stmt s(conn(), "UPDATE maker_quotes SET status = 'cancelled' WHERE id = ?");
        s.bind_int(1, quote_id);
        s.step();
    }
    return get_maker_quote(quote_id);
}

MakerQuote Database::update_maker_quote_accrual(int quote_id, const AccrualUpdate& u) {
    {
        Stmt s(conn(),
               "UPDATE maker_quotes SET accrued_rewards = ?, realized_bleed = ?, fills = ?, "
               "last_mid = ?, last_accrued_at = ?, inventory = ?, inventory_pnl = ?, "
               "complement_inventory = ? WHERE id = ?");
        s.bind_double(1, u.accrued_rewards);
        s.bind_double(2, u.realized_bleed);
        s.bind_int(3, u.fills);
        s.bind_double(4, u.last_mid);
        s.bind_text(5, u.last_accrued_at);
        s.bind_double(6, u.inventory);
        s.bind_double(7, u.inventory_pnl);
        s.bind_double(8, u.complement_inventory);
        s.bind_int(9, quote_id);
        s.step();
    }
    return *get_maker_quote(quote_id);
}

}  // namespace pmm
