// src/pmm/api.cpp — Polymarket Gamma + CLOB 客户端实现 (port of pm_trader/api.py)
#include "pmm/api.hpp"

#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "pmm/jsonutil.hpp"

namespace pmm {

namespace {

constexpr char kGammaHost[] = "gamma-api.polymarket.com";
constexpr char kClobHost[] = "clob.polymarket.com";
constexpr char kDataApiHost[] = "data-api.polymarket.com";  // 链上真实持仓 (启动对账, 无鉴权)
constexpr int kCacheTtlSeconds = 300;

namespace ju = pmm::jsonutil;
using nlohmann::json;

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string build_query(const std::vector<std::pair<std::string, std::string>>& params) {
    if (params.empty()) return {};
    std::string q = "?";
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) q += "&";
        first = false;
        q += url_encode(k) + "=" + url_encode(v);
    }
    return q;
}

}  // namespace

// ---------------------------------------------------------------------------
// 解析助手 (纯函数)
// ---------------------------------------------------------------------------

bool has_condition_id(const json& data) {
    if (!data.is_object()) return false;
    const json* a = ju::find(data, "conditionId");
    const json* b = ju::find(data, "condition_id");
    return (a != nullptr && ju::truthy(*a)) || (b != nullptr && ju::truthy(*b));
}

Market parse_clob_market(const json& data) {
    Market m;
    json tokens_raw = json::array();
    if (const json* t = ju::find(data, "tokens")) tokens_raw = ju::as_container(*t);

    for (const auto& t : tokens_raw) {
        Token tok;
        if (const json* v = ju::find(t, "token_id")) tok.token_id = ju::to_str(*v);
        if (const json* v = ju::find(t, "outcome")) tok.outcome = ju::to_str(*v);
        m.tokens.push_back(std::move(tok));
    }

    for (const auto& tok : m.tokens) m.outcomes.push_back(tok.outcome);
    if (m.outcomes.empty()) m.outcomes = {"Yes", "No"};
    m.outcome_prices = {0.0, 0.0};  // CLOB 此处不返回价格

    if (const json* v = ju::find(data, "condition_id")) m.condition_id = ju::to_str(*v);
    if (const json* v = ju::find(data, "market_slug")) m.slug = ju::to_str(*v);
    if (const json* v = ju::find(data, "question")) m.question = ju::to_str(*v);
    if (const json* v = ju::find(data, "description")) m.description = ju::to_str(*v);
    m.active = data.contains("active") ? ju::str_bool(data["active"]) : true;
    m.closed = data.contains("closed") ? ju::str_bool(data["closed"]) : false;
    if (const json* v = ju::find(data, "end_date_iso")) m.end_date = ju::to_str(*v);
    if (const json* v = ju::find(data, "minimum_tick_size")) {
        const double t = ju::to_double(*v);
        m.tick_size = t != 0.0 ? t : 0.01;
    } else {
        m.tick_size = 0.01;
    }
    return m;
}

Market parse_market(const json& data) {
    Market m;

    // outcomes (可为 JSON 字符串或数组)
    std::vector<std::string> outcomes;
    if (const json* o = ju::find(data, "outcomes")) {
        for (const auto& x : ju::as_container(*o)) outcomes.push_back(ju::to_str(x));
    }
    if (outcomes.empty()) outcomes = {"Yes", "No"};
    m.outcomes = outcomes;

    // outcome_prices (camelCase outcomePrices / snake; 可为字符串或数组; 值可为字符串数字)
    std::vector<double> prices;
    if (const json* p = ju::find(data, "outcomePrices", "outcome_prices")) {
        for (const auto& x : ju::as_container(*p)) prices.push_back(ju::to_double(x));
    }
    m.outcome_prices = prices.empty() ? std::vector<double>{0.0, 0.0} : prices;

    // tokens: 优先 clobTokenIds (JSON 字符串的 id 数组, 顺序对应 outcomes); 否则 tokens 列表
    if (const json* cids = ju::find(data, "clobTokenIds")) {
        const json arr = ju::as_container(*cids);
        for (std::size_t i = 0; i < arr.size(); ++i) {
            Token tok;
            tok.token_id = ju::to_str(arr[i]);
            tok.outcome = i < outcomes.size() ? outcomes[i] : ("Outcome" + std::to_string(i));
            m.tokens.push_back(std::move(tok));
        }
    } else if (const json* tks = ju::find(data, "tokens")) {
        for (const auto& t : ju::as_container(*tks)) {
            Token tok;
            if (const json* v = ju::find(t, "token_id")) tok.token_id = ju::to_str(*v);
            if (const json* v = ju::find(t, "outcome")) tok.outcome = ju::to_str(*v);
            m.tokens.push_back(std::move(tok));
        }
    }

    if (const json* v = ju::find(data, "conditionId", "condition_id")) m.condition_id = ju::to_str(*v);
    if (const json* v = ju::find(data, "slug")) m.slug = ju::to_str(*v);
    if (const json* v = ju::find(data, "question")) m.question = ju::to_str(*v);
    if (const json* v = ju::find(data, "description")) m.description = ju::to_str(*v);
    m.active = data.contains("active") ? ju::py_bool(data["active"]) : false;
    m.closed = data.contains("closed") ? ju::py_bool(data["closed"]) : false;
    if (const json* v = ju::find(data, "volume")) m.volume = ju::to_double(*v);
    if (const json* v = ju::find(data, "liquidity")) m.liquidity = ju::to_double(*v);
    if (const json* v = ju::find(data, "endDateIso", "end_date_iso")) {
        m.end_date = ju::to_str(*v);
    } else if (const json* v = ju::find(data, "end_date")) {
        m.end_date = ju::to_str(*v);
    }
    if (const json* v = ju::find(data, "fee_rate_bps")) m.fee_rate_bps = static_cast<int>(ju::to_double(*v));
    // tick: orderPriceMinTickSize / minimum_tick_size, 默认 0.01
    if (const json* v = ju::find(data, "orderPriceMinTickSize", "minimum_tick_size")) {
        const double t = ju::to_double(*v);
        m.tick_size = t != 0.0 ? t : 0.01;
    } else {
        m.tick_size = 0.01;
    }
    return m;
}

OrderBook parse_order_book(const json& data) {
    OrderBook ob;
    if (const json* bids = ju::find(data, "bids")) {
        if (bids->is_array()) {
            for (const auto& e : *bids) {
                OrderBookLevel lvl;
                if (const json* v = ju::find(e, "price")) lvl.price = ju::to_double(*v);
                if (const json* v = ju::find(e, "size")) lvl.size = ju::to_double(*v);
                ob.bids.push_back(lvl);
            }
        }
    }
    if (const json* asks = ju::find(data, "asks")) {
        if (asks->is_array()) {
            for (const auto& e : *asks) {
                OrderBookLevel lvl;
                if (const json* v = ju::find(e, "price")) lvl.price = ju::to_double(*v);
                if (const json* v = ju::find(e, "size")) lvl.size = ju::to_double(*v);
                ob.asks.push_back(lvl);
            }
        }
    }
    return ob;
}

namespace {

std::vector<Market> parse_market_list(const json& data) {
    std::vector<Market> out;
    if (!data.is_array()) return out;
    for (const auto& m : data) {
        if (has_condition_id(m)) out.push_back(parse_market(m));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// PolymarketClient
// ---------------------------------------------------------------------------

PolymarketClient::PolymarketClient(Database& db, RateLimiter* rate_limiter, std::size_t pool_size)
    : db_(db),
      rate_limiter_(rate_limiter),
      gamma_(kGammaHost, pool_size),
      clob_(kClobHost, pool_size),
      data_api_(kDataApiHost, pool_size) {}

// 链上真实持仓 (token -> 净 size); 启动对账用, 把账本校正到现实 (清幻象 + 找回真实仓)。无鉴权。
std::map<std::string, double> PolymarketClient::chain_positions(const std::string& user) {
    std::map<std::string, double> out;
    if (user.empty()) return out;
    try {
        const net::HttpResponse r = data_api_.Get("/positions?user=" + user + "&sizeThreshold=0.5");
        if (r.status == 0 || r.status >= 400) return out;
        const json data = json::parse(r.body);
        if (!data.is_array()) return out;
        for (const auto& p : data) {
            const std::string asset = p.value("asset", std::string{});
            const double size = p.value("size", 0.0);
            if (!asset.empty() && std::abs(size) >= 0.5) out[asset] = size;
        }
    } catch (...) {
    }
    return out;
}

// 链上真实持仓总市值 (Σ size×curPrice); 急停用真实净值 = USDC + 此值 (持仓不算亏, 只有逆向跌价才算)。
double PolymarketClient::chain_position_value(const std::string& user) {
    if (user.empty()) return 0.0;
    double total = 0.0;
    try {
        const net::HttpResponse r = data_api_.Get("/positions?user=" + user + "&sizeThreshold=0.5");
        if (r.status == 0 || r.status >= 400) return 0.0;
        const json data = json::parse(r.body);
        if (!data.is_array()) return 0.0;
        for (const auto& p : data) {
            const double size = p.value("size", 0.0);
            const double cur = p.value("curPrice", 0.0);
            if (std::abs(size) >= 0.5 && cur > 0.0) total += size * cur;
        }
    } catch (...) {
    }
    return total;
}

json PolymarketClient::gamma_markets_raw(const std::vector<std::pair<std::string, std::string>>& params) {
    try {
        const json d = gamma_get("/markets", params);
        return d.is_array() ? d : json::array();
    } catch (...) {
        return json::array();
    }
}

json PolymarketClient::gamma_get(const std::string& path, const Params& params) {
    const net::HttpResponse r = gamma_.Get(path + build_query(params));
    if (r.status == 0) throw ApiError("Gamma API request failed");
    if (r.status >= 400) {
        throw ApiError(std::format("Gamma API error: {} {}", r.status, r.body.substr(0, 200)),
                       r.status);
    }
    try {
        return json::parse(r.body);
    } catch (...) {
        throw ApiError("Gamma API: invalid JSON response");
    }
}

json PolymarketClient::clob_get(const std::string& path, const Params& params) {
    if (rate_limiter_ != nullptr) {
        rate_limiter_->acquire(path, "GET");  // 按端点分类 (官方 per-endpoint 限额)
    }
    const net::HttpResponse r = clob_.Get(path + build_query(params));
    if (r.status == 0) throw ApiError("CLOB API request failed");
    if (r.status >= 400) {
        throw ApiError(std::format("CLOB API error: {} {}", r.status, r.body.substr(0, 200)),
                       r.status);
    }
    try {
        return json::parse(r.body);
    } catch (...) {
        throw ApiError("CLOB API: invalid JSON response");
    }
}

std::optional<json> PolymarketClient::get_cached(const std::string& key) {
    auto raw = db_.get_cache_fresh(key, kCacheTtlSeconds);
    if (!raw) return std::nullopt;
    try {
        return json::parse(*raw);
    } catch (...) {
        return std::nullopt;
    }
}

void PolymarketClient::set_cached(const std::string& key, const json& data) {
    db_.set_cache(key, data.dump());
}

Market PolymarketClient::get_market(const std::string& slug_or_id) {
    const std::string cache_key = "market:" + slug_or_id;
    if (auto cached = get_cached(cache_key)) return parse_market(*cached);

    // 先按 slug 试 (Gamma)
    const json data = gamma_get("/markets", {{"slug", slug_or_id}});
    if (data.is_array() && !data.empty()) {
        set_cached(cache_key, data[0]);
        return parse_market(data[0]);
    }
    if (data.is_object() && has_condition_id(data)) {
        set_cached(cache_key, data);
        return parse_market(data);
    }

    // 再按 condition_id 试 (CLOB 精确匹配)
    if (slug_or_id.rfind("0x", 0) == 0) {
        try {
            const json clob_data = clob_get("/markets/" + slug_or_id);
            if (clob_data.is_object()) {
                const json* cid = ju::find(clob_data, "condition_id");
                if (cid != nullptr && ju::truthy(*cid)) {
                    Market market = parse_clob_market(clob_data);
                    if (!market.slug.empty()) {
                        try {
                            const json gamma_data = gamma_get("/markets", {{"slug", market.slug}});
                            if (gamma_data.is_array() && !gamma_data.empty()) {
                                set_cached(cache_key, gamma_data[0]);
                                return parse_market(gamma_data[0]);
                            }
                        } catch (...) {
                        }
                    }
                    set_cached(cache_key, clob_data);
                    return market;
                }
            }
        } catch (const ApiError&) {
        }
    }

    throw MarketNotFoundError(slug_or_id);
}

std::vector<Market> PolymarketClient::list_markets(int limit, const std::string& sort_by) {
    Params params = {{"limit", std::to_string(limit)}, {"active", "true"}, {"closed", "false"}};
    if (sort_by == "volume") {
        params.push_back({"order", "volume"});
        params.push_back({"ascending", "false"});
    } else if (sort_by == "liquidity") {
        params.push_back({"order", "liquidity"});
        params.push_back({"ascending", "false"});
    }
    return parse_market_list(gamma_get("/markets", params));
}

std::vector<Market> PolymarketClient::search_markets(const std::string& query, int limit) {
    return parse_market_list(gamma_get("/markets", {{"_q", query}, {"limit", std::to_string(limit)}}));
}

json PolymarketClient::get_tags() {
    const std::string cache_key = "tags:all";
    if (auto cached = get_cached(cache_key)) return *cached;
    const json data = gamma_get("/tags");
    if (!data.is_array() || data.empty()) return json::array();
    set_cached(cache_key, data);
    return data;
}

std::vector<Market> PolymarketClient::get_markets_by_tag(const std::string& tag_slug, int limit,
                                                         bool closed) {
    Params params = {{"tag_slug", tag_slug},
                     {"limit", std::to_string(limit)},
                     {"closed", closed ? "true" : "false"},
                     {"active", closed ? "false" : "true"}};
    return parse_market_list(gamma_get("/markets", params));
}

json PolymarketClient::get_event(const std::string& slug) {
    const std::string cache_key = "event:" + slug;
    if (auto cached = get_cached(cache_key)) return *cached;
    const json data = gamma_get("/events/" + slug);
    if (data.is_object()) {
        set_cached(cache_key, data);
        return data;
    }
    return json::object();
}

OrderBook PolymarketClient::get_order_book(const std::string& token_id) {
    return parse_order_book(clob_get("/book", {{"token_id", token_id}}));
}

double PolymarketClient::get_midpoint(const std::string& token_id) {
    const json data = clob_get("/midpoint", {{"token_id", token_id}});
    if (const json* v = ju::find(data, "mid")) return ju::to_double(*v);
    return 0.0;
}

int PolymarketClient::get_fee_rate(const std::string& token_id) {
    const std::string cache_key = "fee_rate:" + token_id;
    if (auto cached = get_cached(cache_key)) {
        if (const json* v = ju::find(*cached, "fee_rate_bps")) return static_cast<int>(ju::to_double(*v));
        return 0;
    }
    const json data = clob_get("/fee-rate", {{"token_id", token_id}});
    int fee_bps = 0;
    if (const json* v = ju::find(data, "fee_rate_bps")) fee_bps = static_cast<int>(ju::to_double(*v));
    set_cached(cache_key, json{{"fee_rate_bps", fee_bps}});
    return fee_bps;
}

double PolymarketClient::get_tick_size(const std::string& token_id) {
    const std::string cache_key = "tick_size:" + token_id;
    if (auto cached = get_cached(cache_key)) {
        if (const json* v = ju::find(*cached, "minimum_tick_size")) return ju::to_double(*v);
        return 0.01;
    }
    const json data = clob_get("/tick-size", {{"token_id", token_id}});
    double tick = 0.01;
    if (const json* v = ju::find(data, "minimum_tick_size")) tick = ju::to_double(*v);
    set_cached(cache_key, json{{"minimum_tick_size", tick}});
    return tick;
}

std::vector<orderbook::PricePoint> PolymarketClient::prices_history(const std::string& token_id,
                                                                    const std::string& interval,
                                                                    int fidelity) {
    const json data = clob_get("/prices-history", {{"market", token_id},
                                                   {"interval", interval},
                                                   {"fidelity", std::to_string(fidelity)}});
    std::vector<orderbook::PricePoint> out;
    if (data.is_object()) {
        if (const json* h = ju::find(data, "history")) {
            if (h->is_array()) {
                for (const auto& pt : *h) {
                    const json* p = ju::find(pt, "p");
                    const json* t = ju::find(pt, "t");
                    if (p == nullptr || t == nullptr) continue;
                    out.push_back({ju::to_double(*p), ju::to_double(*t)});
                }
            }
        }
    }
    return out;
}

std::optional<rewards::RewardConfig> PolymarketClient::get_reward_config(
    const std::string& condition_id) {
    const json data = clob_get("/markets/" + condition_id);
    if (!data.is_object()) return std::nullopt;
    return rewards::parse_rewards(data);
}

std::tuple<Market, OrderBook, int> PolymarketClient::get_trade_context(const std::string& slug_or_id,
                                                                       const std::string& outcome) {
    Market market = get_market(slug_or_id);
    const std::string token_id = market.get_token_id(outcome);
    OrderBook book = get_order_book(token_id);
    const int fee_rate = get_fee_rate(token_id);
    return {std::move(market), std::move(book), fee_rate};
}

}  // namespace pmm
