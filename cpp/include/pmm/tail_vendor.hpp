// pmm/tail_vendor.hpp — 系统性上行尾卖方 (sell UP-tail lottery tickets: rest GTC BUY NO, hold to
// resolution). 策略依据 = 2026-07-03 校准 (32,570 个已结算市场/18 个月): 上行尾 (YES 2-7c) 在
// 全部行情周期被高估 4-5.5x; 下行尾与 touch 家族定价公允 — 不碰。
//
// 设计: 决策核是纯函数 (parse_candidate / decide, 离线可单测, 零 IO); 扫描/下单/对账在 app 层
// 复用 PolymarketClient + ClobSubmitter。慢频策略 (分钟级) — 性能 = 少请求 + 连接复用 + 无忙等。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

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
    bool band_clamp{false};  // 策展授权"首卖": ask 肥/空书时允许 clamp 到带顶 yes_max 当第一个卖家
                             // (授权条件在 curator 端: 锚定 fair≤2% 或无锚但 bid≤4c 确认深尾)
    bool is_touch{false};    // curator anchor=touch-model — 触碰/reach 卫星腿, 走独立小预算 + per-row sizing
    double wl_collateral{0}; // 白名单 detail 提供的 per-row 抵押 $ (触碰腿风险平价); 0 = 用 cfg 默认
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
    double per_coin_usd{60};    // 单币种已部署 (resting+filled) 抵押上限 (锚定核心 btc/eth)
    double per_coin_alt_usd{0}; // 无锚段 (sol/xrp) 单币上限; 0 = 回落 per_coin_usd (向后兼容)。
                                // 边际未经 Deribit 锚验证 + 已实盘命中 -> 收紧, 防 mania 流量铺满不安全段。
    double per_market_usd{25};  // 单市场已部署上限 (分散优先: 防同一买家反复加注同一盘吃穿币种额度;
                                // 默认须 ≥ per_order 单笔名义, 否则一单都下不出)
    double total_usd{60};       // 总抵押上限
    int max_orders{8};
    double min_shares{5};       // PM 最小单
    double tick{0.001};
    // 触碰/reach 卫星腿的独立预算 (与上面的 core 上限完全隔离; core 永不吃触碰额度, 反之亦然)。
    // 默认全 0 = 触碰执行 OFF: 即便白名单含触碰 slug, 未设 touch_total>0 时 decide() 一律拒触碰单。
    // per-order 抵押来自白名单 (Candidate::wl_collateral), 这里是执行侧硬顶 (env 只能调低)。
    double touch_per_order_usd{0};   // 单触碰仓抵押上限 (curator 送 ≤24)
    double touch_per_market_usd{0};  // 单触碰市场 (= 一个 reach 行权价) 上限
    double touch_per_coin_usd{0};    // 单币触碰已部署上限
    double touch_total_usd{0};       // 触碰总抵押上限 (0 = 触碰执行未武装)
    int touch_max_orders{8};         // 触碰订单数子上限 (core 先铺, 触碰只用剩余名额且不超此数; 防挤占 core)
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
// deployed_market / deployed_coin / deployed_total = 该市场/币种/全局已占用抵押
// (resting notional + held collateral)。
[[nodiscard]] std::optional<Quote> decide(const Candidate& c, const Config& cfg,
                                          double deployed_market, double deployed_coin,
                                          double deployed_total);

// 持单退出判断: true = 该撤 (带外/逼近障碍/临期)。纯函数。
[[nodiscard]] bool should_pull(const Candidate& c, const Config& cfg);

// 下单 response 分类 (纯函数, 可单测): 传输层 (http==0 超时/断连) 或服务端 5xx = 瞬时故障 ->
// 只退避重试, 绝不计入 3-连拒 halt; 唯有真实 4xx 业务拒单 (坏定价/授权/余额) 才 halt 保全现场。
// 2026-07-06 事故根因: 24s 网络抖动连拒 3 单误触 halt -> 干净退出 8h 无人管。
[[nodiscard]] inline bool is_transient_place_error(int http) { return http == 0 || http >= 500; }

// ---- 一轮的完整决策 (纯函数, app 只执行) ----
struct OpenOrder {
    std::string no_token;
    double no_price{0};
    double size{0};      // 未成交剩余
    std::string coin;    // 可为空 (老单无法归因 -> 只计 total)
    std::string note;
};
struct Action {
    enum class Kind { kCancel, kPlace };
    Kind kind{Kind::kPlace};
    std::string no_token;
    double no_price{0};  // kPlace
    double size{0};      // kPlace
    std::string why;     // kCancel: left_window / pull_signal / outbid
    std::string note;
};
// held_* = 已成交持有的抵押 (app 累计, $1/股保守); resting 从 open 内部推导。
// 输出顺序: 先撤后报; 报单侧在函数内做轮内累计 (per-coin + total + max_orders), 上限永不越。
// held_coin_touch / held_total_touch = 触碰腿已成交抵押 (按 token 归属拆出; 默认空 = 无触碰, 行为不变)。
// core 侧 held_coin/held_total 保持传全量 (含触碰, 保守), 故 core 路径零行为变化。
[[nodiscard]] std::vector<Action> plan(const std::map<std::string, Candidate>& cands,
                                       const std::vector<OpenOrder>& open,
                                       const std::map<std::string, double>& held_coin,
                                       const std::map<std::string, double>& held_token,
                                       double held_total, const Config& cfg,
                                       const std::map<std::string, double>& held_coin_touch = {},
                                       double held_total_touch = 0.0);

// ---- sheet 模式: 精确执行人工指定清单 (绕过扫描/decide, 但硬顶与安全校验同一套) ----
struct SheetEntry {
    std::string no_token;
    double no_price{0};
    double size{0};
    std::string note;
};
// JSON 数组 [{"token_id","price","size","note"}] -> 清单; 字段缺失/类型错的行使解析整体失败 (nullopt)。
[[nodiscard]] std::optional<std::vector<SheetEntry>> parse_sheet(const nlohmann::json& j);
// 校验: 条数/总抵押 ≤ cfg 上限, 价格 ∈ [0.80, 0.995] (买 NO 带), 股数 ∈ [min_shares, 100],
// token 全数字且不重复。返回 nullopt = 通过, 否则错误描述。
[[nodiscard]] std::optional<std::string> validate_sheet(const std::vector<SheetEntry>& sheet,
                                                        const Config& cfg);

}  // namespace pmm::tail
