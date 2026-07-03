// pmm/clob_submitter.hpp — 真实 CLOB 下单器 (port of pm_trader/maker_live.py ClobSubmitter)
//
// 替代 py-clob-client: 用已实盘验证的 V2 签名链 (eip712_v2 + secp256k1 + clob_wire) + libcurl POST。
//   下单: get_order_amounts(price,size)→OrderV2 digest→SignDigest→recover 自检→BuildOrderV2Body
//          →ComputeL2Signature→POST /order。L1 ClobAuth 派生 api creds。
// 硬闸: 仅 PM_TRADER_LIVE=1 + POLYMARKET_PRIVATE_KEY 才构造成功。私钥仅在内存, 绝不 log。
// 实盘路径离线无法测 (同 Python README: 上线前小额 smoke-test 确认)。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/maker_live.hpp"  // ConnectionWarmer
#include "pmm/ratelimit.hpp"
#include "pmm/submitter.hpp"

namespace pmm::clob {

// ---- 金额计算 (port py-clob-client builder.py; 纯函数, 可离线对齐) ----

struct RoundConfig {
    int price{2};
    int size{2};
    int amount{4};
};
// tick_size 字符串 → round decimals 表 ("0.1"/"0.01"/"0.001"/"0.0001"; 其它回退 0.01)。
[[nodiscard]] RoundConfig round_config(const std::string& tick_size);

[[nodiscard]] double round_normal(double x, int n);  // 银行家舍入 (round-half-even)
[[nodiscard]] double round_down(double x, int n);    // floor
[[nodiscard]] double round_up(double x, int n);      // ceil
[[nodiscard]] std::int64_t to_token_decimals(double x);  // x*1e6 取整 (micro 单位)

struct OrderAmounts {
    std::uint64_t maker_amount{0};
    std::uint64_t taker_amount{0};
    int side{0};  // 0=BUY 1=SELL
};
// 限价单: 由 (price, size 份额, side) 算 maker/taker (micro)。
[[nodiscard]] OrderAmounts get_order_amounts(bool is_buy, double size, double price,
                                             const RoundConfig& rc);
// 市价单: BUY 时 amount=USDC, SELL 时 amount=shares。
[[nodiscard]] OrderAmounts get_market_order_amounts(bool is_buy, double amount, double price,
                                                    const RoundConfig& rc);

// ---- 实盘下单器 ----

struct LiveCredentials {
    std::array<std::uint8_t, 32> private_key{};
    std::string maker;       // 0x.. (funder/proxy, EOA 模式 = signer)
    std::string signer_lc;   // 0x.. (EOA, 小写)
    int signature_type{0};   // 0=EOA 1=POLY_PROXY 2=GNOSIS_SAFE
    std::string api_key;
    std::string api_secret;  // base64url
    std::string api_passphrase;
};

class ClobSubmitter : public pmm::ISubmitter {
public:
    explicit ClobSubmitter(RateLimiter* rate_limiter = nullptr,
                           std::string endpoint = "https://clob.polymarket.com");
    ~ClobSubmitter() override;
    ClobSubmitter(const ClobSubmitter&) = delete;
    ClobSubmitter& operator=(const ClobSubmitter&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::string& signer_address() const noexcept { return creds_.signer_lc; }

    // submitter 接口 (LiveMakerBot 的 callable): PLACE / CANCEL_ALL / FLATTEN。永不抛入热环。
    nlohmann::json operator()(const nlohmann::json& action) noexcept;
    nlohmann::json submit(const nlohmann::json& action) override { return (*this)(action); }

    // 自上次 poll 起的真实 maker 成交: 每条 {id, token_id, side, size, price}。
    std::vector<nlohmann::json> poll_fills() override;
    // 一次性补账: 比 from_id 更新的全部成交 (newest-first), 不动 poll_fills 的游标。
    // from_id 空 → 返回空 (没有基准就不吞历史)。重启恢复宕机期间成交用 (调用方持久化自己的游标)。
    std::vector<nlohmann::json> fills_since(const std::string& from_id);
    // 纯函数 (可测): 从 newest-first 的 trades 数组取比 last_id 更新的成交, 排除 own taker。
    // 副作用: 把 last_id 推进到本轮最新一笔。break 必须对"进入时的旧游标"比, 不能对循环里刚更新的。
    // maker_lc 非空 → 按 maker_orders[] 过滤出我们的腿 (matched_amount/price/asset 的 MAKER 口径;
    // 顶层字段是 taker 视角, 直接用会产生幻影库存)。空 → 旧顶层口径 (兜底/纯 taker 场景)。
    static std::vector<nlohmann::json> extract_new_fills(const nlohmann::json& data,
                                                         std::optional<std::string>& last_id,
                                                         const std::set<std::string>& own_taker,
                                                         bool invert_side,
                                                         const std::string& maker_lc = "");
    // 诊断: 用不同 query 打 /data/trades, 返回每种的 http 状态 + 原始 body 片段 (查 fill 检测用)。
    nlohmann::json debug_trades();
    // 诊断: 最近 n 条完整原始 trade 行 (含 maker_orders[]) — fill 记账问题的第一现场。
    nlohmann::json recent_trades_raw(std::size_t n);
    // 真实奖励感知: /rewards/user/markets (按 date 查真实 earnings + earning_percentage + 配置) +
    // /rewards/user/percentages (实时占比 {cond:%})。date 空 = 当天。L2 鉴权, 只读。
    nlohmann::json query_rewards(const std::string& date = "") override;
    // 钱包自由 USDC (kill-switch 用); 失败 nullopt。
    std::optional<double> usdc_balance() override;
    // L2 creds (WS user channel 用)。
    nlohmann::json api_creds() override;
    // 所有 resting 单 (重启 broker 对账用)。
    std::vector<nlohmann::json> list_open_orders() override;
    void cancel_order(const std::string& order_id) override;
    [[nodiscard]] bool invert_side() const override { return invert_side_; }

    // Pre-warm the per-token caches (tick size + neg-risk) OFF the order hot path. The FIRST buy in a new
    // window otherwise pays 2 network GETs (~5-10ms) — and that latency is exactly what decides fill-vs-
    // FAK-kill when racing for a stale ask. Call at window discovery for both tokens. Safe/idempotent.
    void warm_token(const std::string& token_id) { (void)fetch_tick_size(token_id); (void)fetch_neg_risk(token_id); }

    void close() override;

private:
    struct Resp {
        int status{0};
        std::string body;
    };
    Resp http(const char* method, const std::string& path, const std::vector<std::string>& headers,
              const std::string& body);
    [[nodiscard]] std::vector<std::string> l2_headers(const std::string& method,
                                                      const std::string& path,
                                                      const std::string& body,
                                                      const std::string& ts) const;

    // order_type: "GTC" (resting, default) or "FAK" (fill-and-kill: takes what's immediately available,
    // cancels the remainder — NEVER rests, so a marketable entry can't leave an async-filling orphan).
    nlohmann::json place(const std::string& token_id, const std::string& side, double price,
                         double size, const std::string& order_type = "GTC");
    nlohmann::json cancel_all(const std::string& token_id);
    nlohmann::json flatten(const std::string& token_id, const std::string& side, double size);

    nlohmann::json fetch_trades_raw();  // GET /data/trades?maker_address= → data 数组 (poll/fills_since 共用)
    std::string fetch_tick_size(const std::string& token_id);  // 缓存 300s
    bool fetch_neg_risk(const std::string& token_id);          // 缓存永久
    std::optional<double> fetch_marketable_price(const std::string& token_id, const std::string& side);
    bool derive_api_creds(std::string& err);
    void warm_ping();

    RateLimiter* rate_limiter_;
    std::string endpoint_;
    LiveCredentials creds_;
    double expiry_s_{0.0};
    bool invert_side_{false};
    bool ready_{false};
    std::optional<std::string> last_trade_id_;
    std::set<std::string> own_taker_ids_;     // 自己的 flatten(taker) 单 → 从 fills 排除
    std::deque<std::string> own_taker_fifo_;  // 插入序 (上限回收按最旧淘汰, 真 FIFO)
    void* curl_{nullptr};                  // 持久 keep-alive CURL handle (void* 避免头引 curl.h)
    std::mutex curl_mu_;                    // curl handle 非线程安全 → 串行化
    std::map<std::string, std::pair<std::string, double>> tick_cache_;  // token → (tick, mono_expiry_s)
    std::map<std::string, bool> neg_risk_cache_;
    std::unique_ptr<pmm::maker::ConnectionWarmer> warmer_;
};

}  // namespace pmm::clob
