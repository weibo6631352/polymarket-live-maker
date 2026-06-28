// pmm/rewards.hpp — 流动性奖励池扫描器 (port of pm_trader/rewards.py)
//
// PM 给在 max_spread 内挂的两边限价单发固定日 USDC 池, size-weight ((c-s)/c)^2。扫描器拉
// /sampling-markets, 估算 min_size 两边报价的毛收益, 按跳跃风险 (KILL/WATCH/SAFE) 过滤排序。
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/net/https_pool.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/ratelimit.hpp"

namespace pmm::rewards {

constexpr int SCAN_WORKERS = 16;
constexpr double MIN_DAILY = 50.0;
constexpr double JUMP_KILL_DAYS = 20.0;
constexpr double JUMP_WATCH_DAYS = 7.0;
constexpr double EMPTY_BAND_SHARE = 0.99;

// parse_rewards 的输出 (一个奖励池的配置)。
struct RewardConfig {
    double daily{0.0};
    double max_spread{0.0};
    double min_size{0.0};
    double tick{0.0};
    std::string token;
    std::string question;
    std::string condition_id;
    double end_date_unix{0.0};  // 市场结算时间 (unix); 0=未知。近结算池 = 灾难性事件风险, 剔除。
};

// classify_jump_risk 的输出。
struct JumpRisk {
    std::string verdict;  // "SAFE"/"WATCH"/"KILL"/"no-history"
    int days{0};
    std::optional<double> max_jump_c;
    std::optional<double> daily_vol_c;
    std::optional<double> recent_vol_c;  // 最近 ~7 天波动 (抓 calm-before-catalyst); 过滤用, 不入 parity JSON
    std::optional<double> days_wiped;  // None = inf (reward<=0)
};

// score_pool 的输出 (一个池的评分报告); 一边空簿返回 nullopt。
struct PoolReport {
    std::string question;
    std::string condition_id;
    std::string token;
    double daily{0.0};
    double max_spread_c{0.0};
    double min_size{0.0};
    double tick{0.0};
    double mid{0.0};
    double spread_c{0.0};
    double inband_notional{0.0};
    double share{0.0};
    double min_side_score{0.0};
    bool empty_band{false};
    double reward_per_day{0.0};
    double gross_ann_pct{0.0};
    std::string jump_verdict;
    std::optional<double> max_jump_c;
    std::optional<double> daily_vol_c;
    std::optional<double> recent_vol_c;  // 近窗波动 (过滤用); 不入 parity JSON
    std::optional<double> days_wiped;
    // B: PM 官方权威数据 (来自 /rewards/markets/multi, 选池用); 不入 parity JSON。
    double competitiveness{-1.0};   // PM 竞争度 (越高越拥挤); <0 = 未知
    double remaining_reward{-1.0};  // 池子剩余额度 ($); <0 = 未知, 0..tiny = 快发完
    double volume_24hr{0.0};        // PM 24h 成交额 ($, 流量信号; curator 据此判流动性)
};

// /rewards/markets/multi 每市场的 PM 权威增强数据 (竞争度 + 剩余额度 + 24h 量)。
struct RewardMulti {
    double competitiveness{0.0};
    double remaining{-1.0};
    double volume_24hr{0.0};
};

// ---- 纯函数 ----
[[nodiscard]] std::optional<RewardConfig> parse_rewards(const nlohmann::json& market);
[[nodiscard]] std::pair<double, double> inband_score(const nlohmann::json& levels, double mid,
                                                     double max_spread_cents, bool is_bid);
[[nodiscard]] double reward_share(double min_size, double tick, double max_spread_cents,
                                  double existing_min_side_score);
[[nodiscard]] JumpRisk classify_jump_risk(const std::vector<double>& prices, double reward_per_day,
                                          double min_size, double kill_days = JUMP_KILL_DAYS,
                                          double watch_days = JUMP_WATCH_DAYS);
[[nodiscard]] std::optional<PoolReport> score_pool(const RewardConfig& pool, const nlohmann::json& book,
                                                   const std::vector<orderbook::PricePoint>& history,
                                                   double reward_calib = 1.0);

// ---- HTTP 客户端 (CLOB reward-pool / book / history) ----
class RewardsClient {
public:
    explicit RewardsClient(RateLimiter* rate_limiter = nullptr, std::size_t pool_size = SCAN_WORKERS);

    [[nodiscard]] std::vector<nlohmann::json> sampling_markets(int max_pages = 100);
    // B: /rewards/markets/multi → {condition_id: RewardMulti(竞争度, 剩余额度$, 24h量)}; 选池用 PM 权威数据。
    // max_pages=20: 官方约 1875 个奖励池跨 ~15 页(每页 500); 4 页只覆盖 ~27% → comp/vol 大量缺失。20 页全覆盖。
    [[nodiscard]] std::map<std::string, RewardMulti> reward_markets_multi(int max_pages = 20);
    [[nodiscard]] nlohmann::json book(const std::string& token_id);
    [[nodiscard]] std::vector<orderbook::PricePoint> prices_history(const std::string& token_id,
                                                                    const std::string& interval = "max",
                                                                    int fidelity = 1440);

private:
    nlohmann::json get(const std::string& path);  // 经 rate_limiter (low priority) + 池
    RateLimiter* rate_limiter_;
    pmm::net::HttpsPool clob_;
};

// scan 结果。
struct ScanResult {
    double min_daily{0.0};
    int top{0};
    bool with_jump_risk{true};
    int total_reward_pools{0};
    int pools_scored{0};
    int safe_count{0};
    std::vector<PoolReport> pools;
};

[[nodiscard]] ScanResult scan(RewardsClient& client, double min_daily = MIN_DAILY, int top = 30,
                              bool with_jump_risk = true, double min_days_to_resolution = 0.0,
                              double max_vol_mult = 0.0, double reward_calib = 1.0,
                              const std::set<std::string>& whitelist = {});

}  // namespace pmm::rewards
