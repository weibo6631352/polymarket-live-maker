// pmm/engine.hpp — 交易执行引擎编排 (port of pm_trader/engine.py)
//
// 串起 api + orderbook + db + orders + maker_live。核心是做市报价的 paper/live 两条 accrual 路径
// (reconcile / drift-exit / reward / inventory-skew / re-center)。结果行用 nlohmann::json (对齐 Python dict)。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/api.hpp"
#include "pmm/db.hpp"
#include "pmm/maker_live.hpp"  // Submitter
#include "pmm/models.hpp"
#include "pmm/orders.hpp"
#include "pmm/ws.hpp"  // MarketChannel (book_source)

namespace pmm {

constexpr double MIN_ORDER_USD = 1.0;
constexpr double MAKER_CAP_MULT = 4.0;
constexpr double MAKER_SKEW_STRENGTH = 1.0;
constexpr double MAX_ACCRUAL_SECONDS = 600.0;
constexpr int MAKER_PREFETCH_WORKERS = 16;

// 一笔真实成交 (live 路径的 fills_by_token)。
struct RealFill {
    std::string side;
    double size{0.0};
    double price{0.0};
};
using FillsByToken = std::map<std::string, std::vector<RealFill>>;

struct MakerQuoteOpts {
    std::optional<double> size;
    std::optional<double> half_spread_cents;
    double cancel_efficiency{0.0};
    std::optional<double> max_inventory;
    double skew_strength{MAKER_SKEW_STRENGTH};
    std::optional<double> now_unix;  // 测试可注入; nullopt = 当前 UTC
};

class Engine {
public:
    explicit Engine(std::filesystem::path data_dir);
    void close();

    // ---- account ----
    Account init_account(double balance = 10000.0);
    Account get_account();
    void reset();

    // ---- maker quotes: paper ----
    nlohmann::json place_maker_quote(const std::string& slug_or_id, const std::string& outcome,
                                     const MakerQuoteOpts& opts);
    std::vector<nlohmann::json> accrue_maker_rewards(std::optional<double> now_unix = std::nullopt);

    // ---- maker quotes: live (真实下单 + 真实成交) ----
    nlohmann::json place_maker_quote_live(const std::string& slug_or_id, const maker::Submitter& submitter,
                                          const std::string& outcome, const MakerQuoteOpts& opts);
    std::vector<nlohmann::json> accrue_maker_rewards_live(const maker::Submitter& submitter,
                                                          const FillsByToken& fills_by_token,
                                                          std::optional<double> now_unix = std::nullopt,
                                                          int recenter_ticks = 1,
                                                          const std::set<std::string>* force_recenter = nullptr);

    nlohmann::json suggest_maker_half_spread(const std::string& slug_or_id, const std::string& outcome = "yes",
                                             double cancel_efficiency = 0.0, double poll_seconds = 60.0);
    std::vector<nlohmann::json> get_maker_quotes();
    std::optional<nlohmann::json> cancel_maker_quote(int quote_id);
    nlohmann::json get_maker_summary();

    // ---- equity ----
    double snapshot_equity();
    void set_maker_crossing_cost_c(double c) { maker_crossing_cost_c_ = c; }
    // 注入实时 WS book 源 (新鲜且连接活时优先于 REST; nullptr = 总走 REST)。
    void set_book_source(ws::MarketChannel* s) noexcept { book_source_ = s; }

    // ---- 直接访问 (runner/cli 用) ----
    Database& db() noexcept { return db_; }
    PolymarketClient& api() noexcept { return api_; }

private:
    struct Mark {
        std::string condition_id;
        std::string outcome;
        double price{0.0};
    };
    // 一个 quote 的并发预取结果。
    struct QuoteRead {
        bool config_ok{false};
        std::optional<rewards::RewardConfig> pool;
        bool book_ok{false};
        OrderBook book;
        double mid{0.0};
    };

    Account require_account();
    static std::string validate_outcome(const std::string& outcome, const Market* market);
    [[nodiscard]] static std::optional<double> book_mid(const OrderBook& book);
    void record_equity(const std::optional<Mark>& mark = std::nullopt);
    [[nodiscard]] double committed_maker_capital();
    std::map<int, QuoteRead> prefetch_quote_reads(const std::vector<MakerQuote>& quotes);
    void credit_maker(double amount);
    void exit_maker_quote(const MakerQuote& quote, double mid, const std::string& reason,
                          std::vector<nlohmann::json>& results, double crossing_cost,
                          const nlohmann::json& extra = nlohmann::json::object());
    static bool action_ok(const nlohmann::json& res);
    nlohmann::json flatten_live(const maker::Submitter& submitter, const std::string& token_id,
                                double inventory);

    Database db_;
    PolymarketClient api_;
    double maker_crossing_cost_c_{0.0};
    ws::MarketChannel* book_source_{nullptr};  // 实时 WS book (可选); nullptr = REST
};

// quote → JSON dict (port of _maker_quote_to_dict)。
[[nodiscard]] nlohmann::json maker_quote_to_dict(const MakerQuote& q);

}  // namespace pmm
