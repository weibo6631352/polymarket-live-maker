// pmm/tail_vendor.hpp — 系统性上行尾卖方 (sell UP-tail lottery tickets: rest GTC BUY NO, hold to
// resolution). 策略依据 = 2026-07-03 校准 (32,570 个已结算市场/18 个月): 上行尾 (YES 2-7c) 在
// 全部行情周期被高估 4-5.5x; 下行尾与 touch 家族定价公允 — 不碰。
//
// 设计: 决策核是纯函数 (parse_candidate / decide, 离线可单测, 零 IO); 扫描/下单/对账在 app 层
// 复用 PolymarketClient + ClobSubmitter。慢频策略 (分钟级) — 性能 = 少请求 + 连接复用 + 无忙等。
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace pmm::tail {

// 一个候选上行尾市场 (从 gamma /markets 行解析)。
struct Candidate {
    std::string cond;        // conditionId
    std::string slug;
    std::string yes_token;   // clobTokenIds[0] (outcomes 必须 == ["Yes","No"], 否则拒绝)
    std::string no_token;    // clobTokenIds[1]
    std::string coin;        // btc / eth / sol / xrp
    double yes_bid{0};       // gamma bestBid (YES)
    double yes_ask{1};       // gamma bestAsk (YES)
    double days_left{0};     // endDate - now
};

// 卖价决策 + 风控参数。编译期硬顶在 app 层 (env 只能调低)。
struct Config {
    double yes_min{0.02};       // 只在 YES ∈ [yes_min, yes_max] 带内卖 (校准带)
    double yes_max{0.07};
    double yes_exit{0.10};      // 持单市场 YES mid ≥ 此值 -> 撤单退出 (行情逼近障碍)
    double floor_yes{0.02};     // 永不把 YES 卖得低于此价 (对公允 ~0.3-1% 保 2-6x 边际)
    double days_min{1.0};
    double days_max{6.0};
    double per_order_usd{20};   // 单笔 NO 抵押上限
    double per_coin_usd{60};    // 单币种已部署 (resting+filled) 抵押上限
    double total_usd{60};       // 总抵押上限
    int max_orders{8};
    double min_shares{5};       // PM 最小单
    double tick{0.001};
};

// 报出的单 (BUY NO)。
struct Quote {
    std::string no_token;
    double no_price{0};   // = 1 - sell_yes, tick 取整
    double size{0};       // 股数
    std::string note;     // slug (日志用)
};

// gamma 行 -> 候选; 拒绝: 微市场/touch 家族/下行方向/非白名单币/坏 token 结构。纯函数。
[[nodiscard]] std::optional<Candidate> parse_candidate(const nlohmann::json& gamma_row,
                                                       double now_unix);

// 候选 -> 报单; 拒绝: 带外/期限外/压过买一/触资金上限。纯函数。
// deployed_coin / deployed_total = 该币种/全局已占用抵押 (resting notional + held collateral)。
[[nodiscard]] std::optional<Quote> decide(const Candidate& c, const Config& cfg,
                                          double deployed_coin, double deployed_total);

// 持单退出判断: true = 该撤 (带外/逼近障碍/临期)。纯函数。
[[nodiscard]] bool should_pull(const Candidate& c, const Config& cfg);

}  // namespace pmm::tail
