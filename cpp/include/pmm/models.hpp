// pmm/models.hpp — 数据模型 + 错误类型 (port of pm_trader/models.py)
//
// 纯数据 (header-only): 异常层次 + Market/OrderBook/Trade/Position/Account/Fill 等结构体。
// 是整个系统的数据词汇表; 其余模块都依赖它。字段与 Python dataclass 一一对应。
#pragma once

#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pmm {

// ---------------------------------------------------------------------------
// 错误层次 (对应 models.py 的 SimError 家族)
// ---------------------------------------------------------------------------

class SimError : public std::runtime_error {
public:
    explicit SimError(std::string message)
        : std::runtime_error(message), message_(std::move(message)) {}
    [[nodiscard]] virtual const char* code() const noexcept { return "SIM_ERROR"; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

protected:
    std::string message_;
};

class NotInitializedError : public SimError {
public:
    explicit NotInitializedError(std::string message = "Account not initialized. Run 'pm-trader init' first.")
        : SimError(std::move(message)) {}
    [[nodiscard]] const char* code() const noexcept override { return "NOT_INITIALIZED"; }
};

class InsufficientBalanceError : public SimError {
public:
    InsufficientBalanceError(double required, double available)
        : SimError(std::format("Insufficient balance: need ${:.2f}, have ${:.2f}", required, available)),
          required(required),
          available(available) {}
    [[nodiscard]] const char* code() const noexcept override { return "INSUFFICIENT_BALANCE"; }
    double required;
    double available;
};

class MarketNotFoundError : public SimError {
public:
    explicit MarketNotFoundError(std::string identifier)
        : SimError(std::format("Market not found: {}", identifier)), identifier(std::move(identifier)) {}
    [[nodiscard]] const char* code() const noexcept override { return "MARKET_NOT_FOUND"; }
    std::string identifier;
};

class MarketClosedError : public SimError {
public:
    explicit MarketClosedError(std::string slug)
        : SimError(std::format("Market is closed: {}", slug)), slug(std::move(slug)) {}
    [[nodiscard]] const char* code() const noexcept override { return "MARKET_CLOSED"; }
    std::string slug;
};

class NoPositionError : public SimError {
public:
    NoPositionError(std::string market, std::string outcome)
        : SimError(std::format("No position in {} ({})", market, outcome)),
          market(std::move(market)),
          outcome(std::move(outcome)) {}
    [[nodiscard]] const char* code() const noexcept override { return "NO_POSITION"; }
    std::string market;
    std::string outcome;
};

class InvalidOutcomeError : public SimError {
public:
    explicit InvalidOutcomeError(std::string outcome)
        : SimError(std::format("Invalid outcome: '{}'.", outcome)), outcome(std::move(outcome)) {}
    [[nodiscard]] const char* code() const noexcept override { return "INVALID_OUTCOME"; }
    std::string outcome;
};

class OrderRejectedError : public SimError {
public:
    explicit OrderRejectedError(std::string reason)
        : SimError(std::format("Order rejected: {}", reason)), reason(std::move(reason)) {}
    [[nodiscard]] const char* code() const noexcept override { return "ORDER_REJECTED"; }
    std::string reason;
};

class TickSizeViolationError : public SimError {
public:
    TickSizeViolationError(double price, double tick_size)
        : SimError(std::format("Price {} violates tick size {}", price, tick_size)),
          price(price),
          tick_size(tick_size) {}
    [[nodiscard]] const char* code() const noexcept override { return "TICK_SIZE_VIOLATION"; }
    double price;
    double tick_size;
};

class ApiError : public SimError {
public:
    explicit ApiError(std::string message, std::optional<int> status_code = std::nullopt)
        : SimError(std::move(message)), status_code(status_code) {}
    [[nodiscard]] const char* code() const noexcept override { return "API_ERROR"; }
    std::optional<int> status_code;
};

// ---------------------------------------------------------------------------
// Market
// ---------------------------------------------------------------------------

// tokens 在 Python 里是 list[dict]; 实际只用到 token_id + outcome 两个键。
struct Token {
    std::string token_id;
    std::string outcome;
};

struct Market {
    std::string condition_id;
    std::string slug;
    std::string question;
    std::string description;
    std::vector<std::string> outcomes;
    std::vector<double> outcome_prices;
    std::vector<Token> tokens;
    bool active{false};
    bool closed{false};
    double volume{0.0};
    double liquidity{0.0};
    std::string end_date;
    int fee_rate_bps{0};
    double tick_size{0.01};

    // 任意 outcome 的 token_id (大小写不敏感)。
    [[nodiscard]] std::string get_token_id(const std::string& outcome) const {
        const std::string lower = to_lower(outcome);
        for (const auto& t : tokens) {
            if (to_lower(t.outcome) == lower) return t.token_id;
        }
        throw std::runtime_error(std::format("No token found for outcome '{}'", outcome));
    }
    [[nodiscard]] std::string yes_token_id() const { return get_token_id("yes"); }
    [[nodiscard]] std::string no_token_id() const { return get_token_id("no"); }

    [[nodiscard]] double yes_price() const { return price_for("yes"); }
    [[nodiscard]] double no_price() const { return price_for("no"); }

private:
    [[nodiscard]] double price_for(const std::string& target) const {
        for (std::size_t i = 0; i < outcomes.size() && i < outcome_prices.size(); ++i) {
            if (to_lower(outcomes[i]) == target) return outcome_prices[i];
        }
        return 0.0;
    }
    [[nodiscard]] static std::string to_lower(std::string s) {
        for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
        return s;
    }
};

// ---------------------------------------------------------------------------
// Order book
// ---------------------------------------------------------------------------

struct OrderBookLevel {
    double price{0.0};
    double size{0.0};
};

struct OrderBook {
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
};

// ---------------------------------------------------------------------------
// Fill / execution
// ---------------------------------------------------------------------------

struct Fill {
    double price{0.0};
    double shares{0.0};
    double cost{0.0};
    int level{0};
};

struct FillResult {
    bool filled{false};
    double avg_price{0.0};
    double total_cost{0.0};
    double total_shares{0.0};
    double fee{0.0};
    double slippage_bps{0.0};
    int levels_filled{0};
    bool is_partial{false};
    std::vector<Fill> fills;
};

// ---------------------------------------------------------------------------
// Trade
// ---------------------------------------------------------------------------

struct Trade {
    int id{0};
    std::string market_condition_id;
    std::string market_slug;
    std::string market_question;
    std::string outcome;
    std::string side;        // buy / sell
    std::string order_type;  // fok / fak
    double avg_price{0.0};
    double amount_usd{0.0};
    double shares{0.0};
    int fee_rate_bps{0};
    double fee{0.0};
    double slippage{0.0};
    int levels_filled{1};
    bool is_partial{false};
    std::string created_at;
};

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

struct Position {
    std::string market_condition_id;
    std::string market_slug;
    std::string market_question;
    std::string outcome;
    double shares{0.0};
    double avg_entry_price{0.0};
    double total_cost{0.0};
    double realized_pnl{0.0};
    bool is_resolved{false};
    std::optional<std::string> resolved_at;

    [[nodiscard]] double current_value(double live_price) const { return shares * live_price; }
    [[nodiscard]] double unrealized_pnl(double live_price) const {
        return current_value(live_price) - total_cost;
    }
    [[nodiscard]] double percent_pnl(double live_price) const {
        if (total_cost == 0.0) return 0.0;
        return (unrealized_pnl(live_price) / total_cost) * 100.0;
    }
};

// ---------------------------------------------------------------------------
// Account
// ---------------------------------------------------------------------------

struct Account {
    int id{0};
    double starting_balance{0.0};
    double cash{0.0};
    std::string created_at;
};

// ---------------------------------------------------------------------------
// Result wrappers
// ---------------------------------------------------------------------------

struct TradeResult {
    Trade trade;
    Account account;
};

struct ResolveResult {
    Position position;
    double payout{0.0};
    Account account;
};

}  // namespace pmm
