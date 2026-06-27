// pmm/orderbook.hpp — 订单簿撮合 + 流动性奖励做市数学 (port of pm_trader/orderbook.py)
//
// 纯函数: 逐档 walk 真实订单簿算精确成交/滑点/费用, 以及 PM 奖励份额 / 逆向选择 bleed /
//   库存 skew / Avellaneda-Stoikov 最优半宽。是 1:1 忠实模拟的核心。
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pmm/models.hpp"

namespace pmm::orderbook {

// ---- 费用 (Polymarket 精确公式) ----
[[nodiscard]] double calculate_fee(int fee_rate_bps, double price, double size);

// ---- 买入模拟 (walk ASK 侧) ----
[[nodiscard]] FillResult simulate_buy_fill(const OrderBook& book, double amount_usd, int fee_rate_bps,
                                           const std::string& order_type = "fok",
                                           std::optional<double> max_price = std::nullopt);

// ---- 卖出模拟 (walk BID 侧) ----
[[nodiscard]] FillResult simulate_sell_fill(const OrderBook& book, double shares, int fee_rate_bps,
                                            const std::string& order_type = "fok",
                                            std::optional<double> min_price = std::nullopt);

// ---- 奖励做市数学 ----

// 官方 PM 单边除数: 中段中点单边/不平衡流动性按 1/c 计分 (c=3)。
inline constexpr double SINGLE_SIDED_DIVISOR = 3.0;

// 两个侧分折成官方绑定 Qmin: 中点 [0.10,0.90] -> max(min,max/3) (单边信用); 极端 -> min。
[[nodiscard]] double binding_qmin(double bid_score, double ask_score, double mid);

// in-band 奖励分: 现存竞争 makers 的绑定 Qmin (经 binding_qmin 折叠)。
[[nodiscard]] double book_inband_qmin(const OrderBook& book, double mid, double max_spread_c);

// 盘口预测信号 (从全深度盘口算, 纯本地零请求)。micro_price 领先 mid; obi>0=上行压力。
struct BookSignals {
    double micro_price{0.0};  // (b0*A0 + a0*B0)/(A0+B0): 按对侧 size 加权的"公允值", 领先 mid
    double obi1{0.0};         // 顶档不平衡 (B0-A0)/(B0+A0) ∈ [-1,1]; 薄盘口噪声大
    double obi_band{0.0};     // 带内 W(s) 加权不平衡; 更稳健
    double depth{0.0};        // 带内总加权深度 (信号质量门: 太小则不可信)
    bool valid{false};        // 双边非空才 true
};
[[nodiscard]] BookSignals compute_book_signals(const OrderBook& book, double mid, double max_spread_c);

// 短程期望漂移 μ̂ (¢/cycle): v1 只用 micro-price 领先 × shrink λ, clamp 到 ±s_cents。
[[nodiscard]] double mu_hat(const BookSignals& sig, double mid, double s_cents, double lambda);

// price 处某侧"前方"挂单量 (better_size, at_level_size) — 队列位置/成交概率粗估。
[[nodiscard]] std::pair<double, double> depth_ahead(const OrderBook& book, double price,
                                                    const std::string& side);

// 自己两边报价的绑定侧奖励分 (两侧对称, 取一侧)。
[[nodiscard]] double maker_quote_score(double size, double half_spread_c, double max_spread_c);

// 日池份额 ≈ ours / (ours + existing)。
[[nodiscard]] double maker_reward_share(double size, double half_spread_c, double max_spread_c,
                                        double existing_qmin);

// 在 band 内停留 seconds、给定份额的 USDC 奖励。
[[nodiscard]] double reward_accrual(double share, double daily_rate, double seconds);

// re-center (无库存) maker 的 per-poll 逆向选择损失。
[[nodiscard]] double adverse_bleed(double size, double half_spread_c, double mid_prev, double mid_now,
                                   double cancel_efficiency = 0.0);

// 库存 skew 后的报价中心 (向远离库存方向偏, 促使回到 flat)。
[[nodiscard]] double skewed_center(double mid, double inventory, double size, double half_spread_c,
                                   double skew_strength);

// mid prev→now 移动时, 模拟 skew 报价的成交。返回 (delta_inventory, fill_loss>=0)。
[[nodiscard]] std::pair<double, double> maker_fill(double mid_prev, double mid_now, double inventory,
                                                   double size, double half_spread_c,
                                                   double skew_strength, double cancel_efficiency,
                                                   double max_inventory);

// 两边 size 报价锁定的现金 (= size*(1-2s), 与 mid 无关)。
[[nodiscard]] double committed_capital(double size, double half_spread_c);

// ---- 波动率感知最优报价 (Avellaneda-Stoikov, 适配补贴) ----

// 零均值高斯移动 (std=sigma) 的 E[max(|Δ|-offset, 0)]。
[[nodiscard]] double expected_excess_move(double sigma, double offset);

// CLOB prices-history 点 (p,t)。
struct PricePoint {
    double p{0.0};
    double t{0.0};
};

// 从历史 mid 路径估 per-poll 移动波动 (cents), 按 √-time 缩放到 poll_seconds。
[[nodiscard]] double realized_sigma_c_from_history(const std::vector<PricePoint>& history,
                                                   double poll_seconds);

// optimal_half_spread 的结果 (各字段 round 到 4 位, 与 Python 一致)。
struct OptimalHalfSpread {
    double half_spread_c{0.0};
    double net_per_day{0.0};
    double reward_per_day{0.0};
    double bleed_per_day{0.0};
    double share{0.0};
};

// 网格搜索使 net 日收益最大的半宽 (cents)。
[[nodiscard]] OptimalHalfSpread optimal_half_spread(double daily_rate, double max_spread_c,
                                                    double min_size, double tick_c,
                                                    double existing_qmin, double sigma_c,
                                                    double periods_per_day,
                                                    double cancel_efficiency = 0.0, int grid = 200);

}  // namespace pmm::orderbook
