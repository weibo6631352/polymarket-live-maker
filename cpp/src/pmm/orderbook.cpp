// src/pmm/orderbook.cpp — 订单簿撮合 + 奖励做市数学实现 (port of pm_trader/orderbook.py)
#include "pmm/orderbook.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pmm/models.hpp"

namespace pmm::orderbook {

namespace {

FillResult empty_fill_result() {
    return FillResult{/*filled=*/false,
                      /*avg_price=*/0.0,
                      /*total_cost=*/0.0,
                      /*total_shares=*/0.0,
                      /*fee=*/0.0,
                      /*slippage_bps=*/0.0,
                      /*levels_filled=*/0,
                      /*is_partial=*/false,
                      /*fills=*/{}};
}

// (best_bid + best_ask) / 2, 任一侧空则 nullopt。不假设已排序。
std::optional<double> midpoint(const OrderBook& book) {
    if (book.bids.empty() || book.asks.empty()) return std::nullopt;
    double best_bid = book.bids.front().price;
    for (const auto& lvl : book.bids) best_bid = std::max(best_bid, lvl.price);
    double best_ask = book.asks.front().price;
    for (const auto& lvl : book.asks) best_ask = std::min(best_ask, lvl.price);
    return (best_bid + best_ask) / 2.0;
}

// PM size-weight ((c - s) / c)^2; band 外或 c<=0 返回 0。
double inband_weight(double s_cents, double max_spread_c) {
    if (max_spread_c <= 0.0) return 0.0;
    if (s_cents < -1e-9 || s_cents > max_spread_c + 1e-9) return 0.0;
    const double w = (max_spread_c - s_cents) / max_spread_c;
    return w * w;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    return s;
}

// Python round(x, 4): round-half-to-even。FE_TONEAREST (默认) 即 round-half-to-even。
double round4(double x) {
    return std::nearbyint(x * 10000.0) / 10000.0;
}

}  // namespace

double calculate_fee(int fee_rate_bps, double price, double size) {
    if (fee_rate_bps == 0) return 0.0;
    double fee = (static_cast<double>(fee_rate_bps) / 10000.0) * std::min(price, 1.0 - price) * size;
    if (fee > 0.0) fee = std::max(fee, 0.0001);
    return fee;
}

FillResult simulate_buy_fill(const OrderBook& book, double amount_usd, int fee_rate_bps,
                             const std::string& order_type, std::optional<double> max_price) {
    if (book.asks.empty()) return empty_fill_result();

    std::vector<OrderBookLevel> sorted_asks = book.asks;
    std::sort(sorted_asks.begin(), sorted_asks.end(),
              [](const OrderBookLevel& a, const OrderBookLevel& b) { return a.price < b.price; });

    double remaining_usd = amount_usd;
    std::vector<Fill> fills;

    for (std::size_t level_idx = 0; level_idx < sorted_asks.size(); ++level_idx) {
        const OrderBookLevel& level = sorted_asks[level_idx];
        if (remaining_usd <= 0.0) break;
        if (max_price.has_value() && level.price > *max_price) break;

        const double max_cost_at_level = level.size * level.price;
        if (max_cost_at_level <= remaining_usd) {
            fills.push_back(Fill{level.price, level.size, max_cost_at_level,
                                 static_cast<int>(level_idx) + 1});
            remaining_usd -= max_cost_at_level;
        } else {
            const double shares = remaining_usd / level.price;
            fills.push_back(Fill{level.price, shares, remaining_usd, static_cast<int>(level_idx) + 1});
            remaining_usd = 0.0;
            break;
        }
    }

    if (fills.empty()) return empty_fill_result();

    double total_cost = 0.0;
    double total_shares = 0.0;
    for (const auto& f : fills) {
        total_cost += f.cost;
        total_shares += f.shares;
    }

    const bool is_partial = remaining_usd > 0.0;
    if (order_type == "fok" && is_partial) return empty_fill_result();

    const double avg_price = total_shares > 0.0 ? total_cost / total_shares : 0.0;
    const double fee = calculate_fee(fee_rate_bps, avg_price, total_cost);

    double slippage_bps = 0.0;
    const auto mid = midpoint(book);
    if (mid.has_value() && *mid > 0.0) {
        slippage_bps = (avg_price - *mid) / *mid * 10000.0;
    }

    return FillResult{!is_partial,
                      avg_price,
                      total_cost,
                      total_shares,
                      fee,
                      slippage_bps,
                      static_cast<int>(fills.size()),
                      is_partial,
                      std::move(fills)};
}

double binding_qmin(double bid_score, double ask_score, double mid) {
    // 官方 PM: 中段中点 [0.10,0.90] 单边/不平衡流动性按 1/c (c=3) 计分: max(min, max/3);
    // 极端 (<0.10 或 >0.90) 必须双边: min。注: 官方按每个 maker 自己的 Qmin 求和, 匿名 L2 只
    // 暴露聚合盘口, 故对聚合盘口套用此规则 (公开数据下最接近的代理)。
    const double lo = std::min(bid_score, ask_score);
    if (mid >= 0.10 && mid <= 0.90) {
        return std::max(lo, std::max(bid_score, ask_score) / SINGLE_SIDED_DIVISOR);
    }
    return lo;
}

double book_inband_qmin(const OrderBook& book, double mid, double max_spread_c) {
    double bid_score = 0.0;
    for (const auto& lvl : book.bids) {
        bid_score += lvl.size * inband_weight((mid - lvl.price) * 100.0, max_spread_c);
    }
    double ask_score = 0.0;
    for (const auto& lvl : book.asks) {
        ask_score += lvl.size * inband_weight((lvl.price - mid) * 100.0, max_spread_c);
    }
    return binding_qmin(bid_score, ask_score, mid);
}

std::pair<double, double> depth_ahead(const OrderBook& book, double price, const std::string& side) {
    constexpr double eps = 1e-9;
    const bool is_bid = (lower(side) == "bid" || lower(side) == "buy");
    const std::vector<OrderBookLevel>& levels = is_bid ? book.bids : book.asks;
    double better = 0.0;
    double at = 0.0;
    for (const auto& lvl : levels) {
        if (std::abs(lvl.price - price) <= eps) {
            at += lvl.size;
        } else if (is_bid ? (lvl.price > price) : (lvl.price < price)) {
            better += lvl.size;
        }
    }
    return {better, at};
}

double maker_quote_score(double size, double half_spread_c, double max_spread_c) {
    return size * inband_weight(half_spread_c, max_spread_c);
}

double maker_reward_share(double size, double half_spread_c, double max_spread_c, double existing_qmin) {
    const double mine = maker_quote_score(size, half_spread_c, max_spread_c);
    const double denom = mine + existing_qmin;
    return denom > 0.0 ? mine / denom : 0.0;
}

double reward_accrual(double share, double daily_rate, double seconds) {
    if (share <= 0.0 || daily_rate <= 0.0 || seconds <= 0.0) return 0.0;
    return share * daily_rate * (seconds / 86400.0);
}

double adverse_bleed(double size, double half_spread_c, double mid_prev, double mid_now,
                     double cancel_efficiency) {
    const double offset = half_spread_c / 100.0;
    const double excess = std::abs(mid_now - mid_prev) - offset;
    if (excess <= 0.0) return 0.0;
    return size * excess * std::max(0.0, 1.0 - cancel_efficiency);
}

double skewed_center(double mid, double inventory, double size, double half_spread_c,
                     double skew_strength) {
    const double offset = half_spread_c / 100.0;
    return size > 0.0 ? mid - skew_strength * (inventory / size) * offset : mid;
}

std::pair<double, double> maker_fill(double mid_prev, double mid_now, double inventory, double size,
                                     double half_spread_c, double skew_strength,
                                     double cancel_efficiency, double max_inventory) {
    const double offset = half_spread_c / 100.0;
    const double center = skewed_center(mid_prev, inventory, size, half_spread_c, skew_strength);
    const double bid = center - offset;
    const double ask = center + offset;
    double f = size * std::max(0.0, 1.0 - cancel_efficiency);
    if (mid_now <= bid) {  // bid hit → buy
        f = std::min(f, std::max(0.0, max_inventory - inventory));
        return {f, f * (bid - mid_now)};
    }
    if (mid_now >= ask) {  // ask lifted → sell
        f = std::min(f, std::max(0.0, max_inventory + inventory));
        return {-f, f * (mid_now - ask)};
    }
    return {0.0, 0.0};
}

double committed_capital(double size, double half_spread_c) {
    const double cap = size * (1.0 - 2.0 * (half_spread_c / 100.0));
    return cap > 0.0 ? cap : 0.0;
}

double expected_excess_move(double sigma, double offset) {
    if (sigma <= 0.0) return 0.0;
    const double a = offset / sigma;
    const double phi = std::exp(-0.5 * a * a) / std::sqrt(2.0 * std::numbers::pi);
    const double cdf = 0.5 * (1.0 + std::erf(a / std::sqrt(2.0)));
    return std::max(0.0, 2.0 * sigma * phi - 2.0 * offset * (1.0 - cdf));
}

double realized_sigma_c_from_history(const std::vector<PricePoint>& history, double poll_seconds) {
    std::vector<double> prices;
    std::vector<double> ts;
    prices.reserve(history.size());
    ts.reserve(history.size());
    for (const auto& pt : history) {
        prices.push_back(pt.p);
        ts.push_back(pt.t);
    }
    if (prices.size() < 2 || poll_seconds <= 0.0) return 0.0;

    std::vector<double> gaps;
    for (std::size_t i = 1; i < ts.size(); ++i) {
        if (ts[i] > ts[i - 1]) gaps.push_back(ts[i] - ts[i - 1]);
    }
    if (gaps.empty()) return 0.0;
    std::sort(gaps.begin(), gaps.end());
    const double step = gaps[gaps.size() / 2];  // 中位 gap (构造上均为正)

    std::vector<double> diffs_c;
    diffs_c.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i) {
        diffs_c.push_back((prices[i] - prices[i - 1]) * 100.0);
    }
    const double n = static_cast<double>(diffs_c.size());
    double sum = 0.0;
    for (double d : diffs_c) sum += d;
    const double mean = sum / n;
    double var_sum = 0.0;
    for (double d : diffs_c) var_sum += (d - mean) * (d - mean);
    const double var = var_sum / n;
    return std::sqrt(var) * std::sqrt(poll_seconds / step);
}

OptimalHalfSpread optimal_half_spread(double daily_rate, double max_spread_c, double min_size,
                                      double tick_c, double existing_qmin, double sigma_c,
                                      double periods_per_day, double cancel_efficiency, int grid) {
    const double lo = tick_c;
    const double hi = max_spread_c;
    if (grid < 1) grid = 1;

    std::vector<double> candidates;
    if (hi <= lo) {
        candidates.push_back(lo);
    } else {
        const double step = (hi - lo) / grid;
        candidates.reserve(static_cast<std::size_t>(grid) + 1);
        for (int i = 0; i <= grid; ++i) candidates.push_back(lo + i * step);
    }

    bool have_best = false;
    OptimalHalfSpread best;
    // Python 比较 `net (raw) > best["net_per_day"] (已 round)`, 故存 round 后的 best net 作比较基准。
    double best_net_stored = 0.0;
    for (double s : candidates) {
        const double own = maker_quote_score(min_size, s, max_spread_c);
        const double denom = own + existing_qmin;
        const double share = denom > 0.0 ? own / denom : 0.0;
        const double reward = share * daily_rate;
        const double bleed = std::max(0.0, 1.0 - cancel_efficiency) * min_size *
                             (expected_excess_move(sigma_c, s) / 100.0) * periods_per_day;
        const double net = reward - bleed;
        if (!have_best || net > best_net_stored) {
            have_best = true;
            best.half_spread_c = round4(s);
            best.net_per_day = round4(net);
            best.reward_per_day = round4(reward);
            best.bleed_per_day = round4(bleed);
            best.share = round4(share);
            best_net_stored = best.net_per_day;  // = Python best["net_per_day"]
        }
    }
    return best;
}

FillResult simulate_sell_fill(const OrderBook& book, double shares, int fee_rate_bps,
                              const std::string& order_type, std::optional<double> min_price) {
    if (book.bids.empty()) return empty_fill_result();

    std::vector<OrderBookLevel> sorted_bids = book.bids;
    std::sort(sorted_bids.begin(), sorted_bids.end(),
              [](const OrderBookLevel& a, const OrderBookLevel& b) { return a.price > b.price; });

    double remaining_shares = shares;
    std::vector<Fill> fills;

    for (std::size_t level_idx = 0; level_idx < sorted_bids.size(); ++level_idx) {
        const OrderBookLevel& level = sorted_bids[level_idx];
        if (remaining_shares <= 0.0) break;
        if (min_price.has_value() && level.price < *min_price) break;

        if (level.size <= remaining_shares) {
            const double cost = level.size * level.price;
            fills.push_back(Fill{level.price, level.size, cost, static_cast<int>(level_idx) + 1});
            remaining_shares -= level.size;
        } else {
            const double cost = remaining_shares * level.price;
            fills.push_back(Fill{level.price, remaining_shares, cost, static_cast<int>(level_idx) + 1});
            remaining_shares = 0.0;
            break;
        }
    }

    if (fills.empty()) return empty_fill_result();

    double total_cost = 0.0;
    double total_shares = 0.0;
    for (const auto& f : fills) {
        total_cost += f.cost;
        total_shares += f.shares;
    }

    const bool is_partial = remaining_shares > 0.0;
    if (order_type == "fok" && is_partial) return empty_fill_result();

    const double avg_price = total_shares > 0.0 ? total_cost / total_shares : 0.0;
    const double fee = calculate_fee(fee_rate_bps, avg_price, total_shares);

    double slippage_bps = 0.0;
    const auto mid = midpoint(book);
    if (mid.has_value() && *mid > 0.0) {
        slippage_bps = (avg_price - *mid) / *mid * 10000.0;
    }

    return FillResult{!is_partial,
                      avg_price,
                      total_cost,
                      total_shares,
                      fee,
                      slippage_bps,
                      static_cast<int>(fills.size()),
                      is_partial,
                      std::move(fills)};
}

}  // namespace pmm::orderbook
