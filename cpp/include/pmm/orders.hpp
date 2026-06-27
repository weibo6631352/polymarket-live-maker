// pmm/orders.hpp — 限价单 + maker_quotes 数据类型 (port of pm_trader/orders.py)
//
// GTC/GTD 限价单 + 两边流动性奖励报价 (maker_quotes)。存储归 Database; accrual 数学在 orderbook,
// 编排在 engine。
#pragma once

#include <optional>
#include <string>

namespace pmm {

struct LimitOrder {
    int id{0};
    std::string market_slug;
    std::string market_condition_id;
    std::string outcome;
    std::string side;        // buy / sell
    double amount{0.0};      // buy=USD, sell=shares
    double limit_price{0.0};
    std::string order_type;  // gtc / gtd
    std::optional<std::string> expires_at;
    std::string status;      // pending/filled/cancelled/expired/rejected
    std::string created_at;
    std::optional<std::string> filled_at;
};

struct LimitOrderInput {
    std::string market_slug;
    std::string market_condition_id;
    std::string outcome;
    std::string side;
    double amount{0.0};
    double limit_price{0.0};
    std::string order_type{"gtc"};
    std::optional<std::string> expires_at;
};

struct MakerQuote {
    int id{0};
    std::string market_slug;
    std::string market_condition_id;
    std::string outcome;
    std::string token_id;            // YES (bid) 腿的 token
    std::string complement_token_id; // NO (ask=BUY-NO) 腿的 token; 纯 USDC 双边做市
    double size{0.0};
    double half_spread_c{0.0};
    double max_spread_c{0.0};
    double min_size{0.0};
    double daily_rate{0.0};
    double tick{0.0};
    double cancel_efficiency{0.0};
    double max_inventory{0.0};
    double skew_strength{0.0};
    double inventory{0.0};
    double inventory_pnl{0.0};
    double entry_mid{0.0};
    double committed_capital{0.0};
    double accrued_rewards{0.0};
    double realized_bleed{0.0};
    int fills{0};
    std::string status;  // active / cancelled
    double last_mid{0.0};
    std::string created_at;
    std::string last_accrued_at;
};

struct MakerQuoteInput {
    std::string market_slug;
    std::string market_condition_id;
    std::string outcome;
    std::string token_id;
    std::string complement_token_id;  // NO 腿 token (BUY-NO 双边)
    double size{0.0};
    double half_spread_c{0.0};
    double max_spread_c{0.0};
    double min_size{0.0};
    double daily_rate{0.0};
    double tick{0.0};
    double cancel_efficiency{0.0};
    double committed_capital{0.0};
    double last_mid{0.0};
    std::string last_accrued_at;
    double max_inventory{0.0};
    double skew_strength{0.0};
    double entry_mid{0.0};
};

struct AccrualUpdate {
    double accrued_rewards{0.0};
    double realized_bleed{0.0};
    int fills{0};
    double last_mid{0.0};
    std::string last_accrued_at;
    double inventory{0.0};
    double inventory_pnl{0.0};
};

// ISO 时间戳归一: 'Z' 后缀 → '+00:00' (保证 TEXT 排序一致)。
[[nodiscard]] inline std::string normalize_timestamp(const std::string& ts) {
    if (!ts.empty() && ts.back() == 'Z') return ts.substr(0, ts.size() - 1) + "+00:00";
    return ts;
}

// 限价单是否应在 best_price 成交: buy 当 best_ask<=limit; sell 当 best_bid>=limit。
[[nodiscard]] inline bool should_fill(const LimitOrder& o, double best_price) {
    return o.side == "buy" ? (best_price <= o.limit_price) : (best_price >= o.limit_price);
}

}  // namespace pmm
