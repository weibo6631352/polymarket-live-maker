// pmm/api.hpp — Polymarket Gamma + CLOB 公开 API 客户端 (port of pm_trader/api.py)
//
// 跑在复用的 PersistentHttps (经 HttpsPool 并发) 上。市场元数据缓存 5min (SQLite); 价格/订单簿从不缓存。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/db.hpp"
#include "pmm/models.hpp"
#include "pmm/net/https_pool.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/ratelimit.hpp"
#include "pmm/rewards.hpp"

namespace pmm {

// ---- 解析助手 (纯函数, 可离线单测; camelCase + snake_case 双兼容, 字段可为 JSON 字符串或数组) ----
[[nodiscard]] bool has_condition_id(const nlohmann::json& data);
[[nodiscard]] Market parse_market(const nlohmann::json& data);          // Gamma /markets
[[nodiscard]] Market parse_clob_market(const nlohmann::json& data);     // CLOB /markets/{id}
[[nodiscard]] OrderBook parse_order_book(const nlohmann::json& data);   // CLOB /book

class PolymarketClient {
public:
    // rate_limiter 可为空 (= 不限速)。pool_size = 每 host 最大并发连接。
    explicit PolymarketClient(Database& db, RateLimiter* rate_limiter = nullptr,
                              std::size_t pool_size = 8);

    // ---- 市场解析 (slug 或 condition_id) ----
    Market get_market(const std::string& slug_or_id);
    std::vector<Market> list_markets(int limit = 20, const std::string& sort_by = "volume");
    std::vector<Market> search_markets(const std::string& query, int limit = 10);
    nlohmann::json get_tags();  // 数组
    std::vector<Market> get_markets_by_tag(const std::string& tag_slug, int limit = 20,
                                           bool closed = false);
    nlohmann::json get_event(const std::string& slug);  // 对象

    // ---- CLOB: 价格 / 订单簿 / 费率 / tick ----
    OrderBook get_order_book(const std::string& token_id);
    double get_midpoint(const std::string& token_id);
    int get_fee_rate(const std::string& token_id);
    double get_tick_size(const std::string& token_id);
    std::vector<orderbook::PricePoint> prices_history(const std::string& token_id,
                                                      const std::string& interval = "max",
                                                      int fidelity = 60);
    // 流动性奖励配置 (rewards::parse_rewards), 无奖励返回 nullopt。
    std::optional<rewards::RewardConfig> get_reward_config(const std::string& condition_id);

    std::tuple<Market, OrderBook, int> get_trade_context(const std::string& slug_or_id,
                                                         const std::string& outcome);

    // 原始 gamma /events 查询 (tail_vendor 按 series_id 精确扫描用); 返回 JSON 数组, 失败空数组。
    nlohmann::json gamma_events_raw(const std::vector<std::pair<std::string, std::string>>& params);

    // 注入共享 TokenBucket (runner 让 engine 的读路径也走全局 req/s 预算)。
    void set_rate_limiter(RateLimiter* rl) noexcept { rate_limiter_ = rl; }

    // 链上真实持仓 (token -> 净 size); 启动对账用 (清幻象 + 找回真实仓)。无鉴权, 失败返回空。
    [[nodiscard]] std::map<std::string, double> chain_positions(const std::string& user);
    // 链上持仓总市值 (Σ size×curPrice); 急停用真实净值 = USDC + 此值。失败返回 0。
    [[nodiscard]] double chain_position_value(const std::string& user);

private:
    using Params = std::vector<std::pair<std::string, std::string>>;
    nlohmann::json gamma_get(const std::string& path, const Params& params = {});
    nlohmann::json clob_get(const std::string& path, const Params& params = {});
    std::optional<nlohmann::json> get_cached(const std::string& key);
    void set_cached(const std::string& key, const nlohmann::json& data);

    Database& db_;
    RateLimiter* rate_limiter_;
    pmm::net::HttpsPool gamma_;
    pmm::net::HttpsPool clob_;
    pmm::net::HttpsPool data_api_;
};

}  // namespace pmm
