// pmm/db.hpp — SQLite 数据层 (port of pm_trader/db.py)
//
// 每个 account 一个 paper.db; WAL + synchronous=NORMAL (1s 做市热环 per-pool per-poll commit)。
// 表: account / trades / positions / market_cache / equity_curve
//   (limit_orders / maker_quotes 由 orders 模块 lazily 建, 这里只在 reset/init 时清理)。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "pmm/models.hpp"
#include "pmm/orders.hpp"

struct sqlite3;

namespace pmm {

// insert_trade 的入参 (对应 db.py 的 keyword-only 参数)。
struct TradeInput {
    std::string market_condition_id;
    std::string market_slug;
    std::string market_question;
    std::string outcome;
    std::string side;
    std::string order_type;
    double avg_price{0.0};
    double amount_usd{0.0};
    double shares{0.0};
    int fee_rate_bps{0};
    double fee{0.0};
    double slippage{0.0};
    int levels_filled{1};
    bool is_partial{false};
};

// upsert_position 的入参。
struct PositionInput {
    std::string market_condition_id;
    std::string market_slug;
    std::string market_question;
    std::string outcome;
    double shares{0.0};
    double avg_entry_price{0.0};
    double total_cost{0.0};
    double realized_pnl{0.0};
};

class Database {
public:
    explicit Database(std::filesystem::path data_dir);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void close();

    // ---- schema ----
    void init_schema();
    void reset();

    // ---- account ----
    Account init_account(double balance = 10000.0);
    [[nodiscard]] std::optional<Account> get_account();
    void update_cash(double new_cash);

    // ---- equity curve ----
    void record_equity(double equity);
    [[nodiscard]] std::vector<double> get_equity_curve();

    // ---- trades ----
    Trade insert_trade(const TradeInput& t);
    [[nodiscard]] std::vector<Trade> get_trades(int limit = 50);

    // ---- positions ----
    Position upsert_position(const PositionInput& p);
    [[nodiscard]] std::optional<Position> get_position(const std::string& market_condition_id,
                                                       const std::string& outcome);
    [[nodiscard]] std::vector<Position> get_open_positions();
    [[nodiscard]] std::vector<Position> get_positions_for_market(const std::string& market_condition_id);
    Position resolve_position(const std::string& market_condition_id, const std::string& outcome,
                              double payout);

    // ---- cache (存/取已序列化的 JSON 文本; 序列化交给调用方) ----
    void set_cache(const std::string& key, const std::string& json_text);
    [[nodiscard]] std::optional<std::string> get_cache(const std::string& key);
    // 仅当写入在 ttl_seconds 内才返回 (api 元数据 5min TTL 用; UTC 一致比较)。
    [[nodiscard]] std::optional<std::string> get_cache_fresh(const std::string& key, int ttl_seconds);

    // ---- limit orders + maker quotes (port of orders.py; 共享同一 conn) ----
    void init_orders_schema();
    LimitOrder create_order(const LimitOrderInput& o);
    [[nodiscard]] std::vector<LimitOrder> get_pending_orders();
    [[nodiscard]] std::optional<LimitOrder> get_order(int order_id);
    [[nodiscard]] std::optional<LimitOrder> cancel_order(int order_id);
    std::vector<LimitOrder> cancel_all_orders();
    LimitOrder mark_filled(int order_id);
    LimitOrder reject_order(int order_id);
    std::vector<LimitOrder> expire_orders();

    MakerQuote create_maker_quote(const MakerQuoteInput& q);
    [[nodiscard]] std::optional<MakerQuote> get_maker_quote(int quote_id);
    [[nodiscard]] std::vector<MakerQuote> get_active_maker_quotes();
    [[nodiscard]] std::vector<MakerQuote> get_all_maker_quotes();
    [[nodiscard]] std::optional<MakerQuote> cancel_maker_quote(int quote_id);
    MakerQuote update_maker_quote_accrual(int quote_id, const AccrualUpdate& u);

    [[nodiscard]] const std::filesystem::path& db_path() const noexcept { return db_path_; }

private:
    sqlite3* conn();           // lazy open + pragmas
    bool table_exists(const std::string& name);

    std::filesystem::path data_dir_;
    std::filesystem::path db_path_;
    sqlite3* conn_{nullptr};
    long equity_inserts_{0};   // amortizes equity_curve pruning
};

}  // namespace pmm
