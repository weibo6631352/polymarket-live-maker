// pmm/runner.hpp — 自主轮询循环 (port of pm_trader/runner.py LiveRunner)
//
// 驱动 engine 的做市环: discover → reselect(选/上/下池) → reevaluate_held(退化退出) → poll_once(accrue)
// + kill-switch + WS 反射快撤 + 后台 discovery/resync 线程 + 优雅关停 cancel/flatten。依赖可注入 (测试)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/config.hpp"
#include "pmm/engine.hpp"
#include "pmm/events.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/portfolio.hpp"
#include "pmm/ratelimit.hpp"
#include "pmm/rewards.hpp"
#include "pmm/submitter.hpp"
#include "pmm/telemetry/publisher.hpp"
#include "pmm/ws.hpp"

namespace pmm {

constexpr int REEVAL_WORKERS = 16;

class LiveRunner {
public:
    // 注入用于测试; nullptr → 内部按需创建 (生产)。
    explicit LiveRunner(RunnerConfig cfg, Engine* engine = nullptr,
                        rewards::RewardsClient* scanner = nullptr, ISubmitter* submitter = nullptr);
    ~LiveRunner();
    LiveRunner(const LiveRunner&) = delete;
    LiveRunner& operator=(const LiveRunner&) = delete;

    void run();
    void trip_kill(const std::string& reason);

    // ---- 核心步骤 (公开以便单测) ----
    void rediscover();
    void reselect();
    void reevaluate_held();
    std::vector<nlohmann::json> poll_once();
    [[nodiscard]] std::optional<std::string> kill_check();
    void on_ws_price(const std::string& token, double mid);
    void tick_cooldowns();
    void refresh_reflex_refs();
    [[nodiscard]] std::optional<std::string> degrade_reason(const nlohmann::json& fresh);

    // ---- 测试访问/注入 ----
    [[nodiscard]] const std::map<std::string, nlohmann::json>& placed() const { return placed_; }
    [[nodiscard]] std::map<std::string, int>& cooldown() { return cooldown_; }
    [[nodiscard]] bool stopped() const { return stop_.load(); }
    void set_last_scan_ok(double t) { last_scan_ok_ = t; }
    void ensure_engine();  // 公开: 测试先 init account

private:
    void event(const std::string& kind, const nlohmann::json& fields);
    void rehydrate();
    void reconcile_broker_orders();
    void start_ws();
    void start_book_resync();
    void resync_once(const std::string& token);
    void publish_book_l2(const std::string& token);  // OrderBookL2 全档遥测 (resync 源头捕获)
    void publish_risk_equity();                       // EquitySnapshot + Position + RiskState 遥测
    void reflex_pull(const std::string& token, const std::string& reason, double mid);  // 双边拉单 (mid-move/fade 共用)
    void start_discovery_thread();
    [[nodiscard]] bool report_stale();
    [[nodiscard]] std::optional<nlohmann::json> rescore(const std::string& cond, const std::string& token,
                                                        double own_size, double own_hs);
    void exit_held(const std::string& cond, const nlohmann::json& quote, const std::string& reason);
    // 有效白名单成员判定 = cfg_.pool_whitelist (静态种子) ∪ dyn_whitelist_ (curator 经 DDS 实时推)。
    // 所有"白名单绕过启发式"的检查都过这里, 确保 DDS 推入的池与静态名单池享同样豁免。短锁读 dyn_whitelist_。
    [[nodiscard]] bool is_effectively_whitelisted(const std::string& cond);
    [[nodiscard]] bool use_live_path() const;
    bool place(const std::string& cond, const nlohmann::json& pool);  // false = 净边际门跳过 (未下单)
    [[nodiscard]] FillsByToken poll_fills();
    void shutdown();
    nlohmann::json locked_submit(const nlohmann::json& action);
    [[nodiscard]] std::vector<std::string> held_tokens();
    void sync_ws_subscriptions();
    [[nodiscard]] double mid_velocity(const std::string& token);
    [[nodiscard]] Engine& engine() { return *engine_; }
    [[nodiscard]] rewards::RewardsClient& scanner();

    RunnerConfig cfg_;
    Engine* engine_{nullptr};
    std::unique_ptr<Engine> owned_engine_;
    rewards::RewardsClient* scanner_{nullptr};
    std::unique_ptr<rewards::RewardsClient> owned_scanner_;
    ISubmitter* submitter_{nullptr};
    std::unique_ptr<ISubmitter> owned_submitter_;
    std::unique_ptr<RateLimiter> rate_limiter_;
    std::unique_ptr<telemetry::Publisher> publisher_{telemetry::make_publisher()};  // 全量遥测总线
    double run_start_{0.0};   // run() 起始单调时刻 (Heartbeat uptime)
    double last_rps_{0.0};    // 最近盘口刷新率 (Heartbeat book_resync_rps)
    double last_heartbeat_{0.0};  // 上次心跳发布时刻 (节流 ~2s)
    std::atomic<long long> book_seq_{0};  // OrderBookL2 单调序号 (全局; 每 token 子序列仍单调, 供丢更检测)
    double last_tel_check_{0.0};  // dry-live 净值/风险态遥测节流时刻 (live 走 kill_check 的 15s 链上块)
    nlohmann::json config_snapshot_;  // ConfigSnapshot 载荷 (启动构建; 周期重发, 防 DDS 发现竞态丢首发)
    double last_config_emit_{0.0};    // 上次 ConfigSnapshot 发布时刻 (节流 ~30s)
    double last_pooleval_emit_{0.0};  // 上次 PoolEval 全量重发时刻 (节流 ~15s; 让任意时刻 curate_read 拿到全候选)
    std::unique_ptr<EventLog> events_;
    std::mutex submitter_mu_;  // 串行化 submitter 访问 (反射 + poll 线程)

    std::mutex report_mu_;  // 后台 discovery 写 report_/last_scan_ok_, 主环读
    rewards::ScanResult report_;
    // 动态白名单: 外部 curator (LLM session) 经 DDS CuratorCommand 热更 (publisher_ 回调写 / reselect 读,
    // 与 report_ 同锁 report_mu_)。有效白名单 = cfg_.pool_whitelist (静态种子) ∪ dyn_whitelist_ (curator 推)。
    std::set<std::string> dyn_whitelist_;
    std::vector<portfolio::SelectedPool> selected_;
    std::map<std::string, nlohmann::json> placed_;  // cond -> 选中池 dict
    std::map<std::string, double> placed_at_;
    std::map<std::string, int> cooldown_;
    std::optional<double> last_scan_ok_;
    std::atomic<bool> stop_{false};
    std::string kill_reason_;

    std::thread discovery_thread_;
    std::atomic<bool> discovery_stop_{false};
    std::vector<std::thread> resync_threads_;  // 并发拉盘口 (吃满 /book 150/s; 单线程 40ms 串行只 ~25/s)
    std::atomic<long> resync_count_{0};         // 累计 /book 拉取数 (算实际刷新率)

    std::unique_ptr<ws::MarketChannel> market_ch_;
    std::unique_ptr<ws::UserChannel> user_ch_;

    std::map<std::string, double> poll_evt_at_;
    std::map<std::string, std::deque<std::pair<double, double>>> mid_hist_;  // token -> (ts, mid)
    std::map<std::string, double> refresh_at_;
    // token -> EWMA(实测在带份额)。复评据此踢"被竞争稀释成 $0 的死池": share×daily < 门槛 → 退出腾资金。
    // 只在主循环 (poll_once 写 / reevaluate_held 读) 访问, 无并发, 不需锁。
    std::map<std::string, double> share_ewma_;

    std::map<std::string, std::pair<double, double>> reflex_refs_;  // token -> (mid, band)
    std::map<std::string, std::string> reflex_complement_;          // yes_token -> no_token (双边一并撤)
    std::set<std::string> reflex_cancelled_;
    std::map<std::string, double> signal_v_;  // token -> max_spread_c (信号带宽); 与 reflex_refs_ 同锁
    std::mutex reflex_mu_;

    // 盘口信号 (micro-price/OBI) 在 REST resync 上实时算, 供 fade-on-imbalance + 标定日志 (telemetry) 用。
    std::map<std::string, double> signal_log_at_;  // token -> 上次记标定日志的单调时刻 (节流)
    std::map<std::string, double> fade_last_s_;     // token -> 上次 fade 的 mono 秒 (冷却: 防紧抖动)
    std::map<std::string, int> fade_since_poll_;    // token -> 自上次维护轮以来的 fade 次数 (signal_mu_ 下)
    std::mutex signal_mu_;
    std::map<std::string, int> fade_streak_;        // cond -> 连续"本轮有 fade"的 poll 数 (仅主循环访问, 免锁)
    void compute_and_store_signals(const std::string& token);

    // 事件驱动主循环: 盘口移动 (REST resync) / WS reflex 立刻唤醒主循环跑决策 (实时响应, 不等定时器);
    // poll_seconds 仅作兜底心跳。
    std::condition_variable loop_cv_;
    std::mutex loop_mu_;
    std::atomic<bool> loop_wake_{false};
    std::atomic<bool> force_reselect_{false};  // curator 经 DDS 推白名单 → 立即强制 reselect (不等 reeval 周期)
    void wake_loop() {
        loop_wake_.store(true);
        std::lock_guard<std::mutex> lk(loop_mu_);
        loop_cv_.notify_one();
    }

    std::set<std::string> seen_fill_ids_;     // 已计成交 id (WS+REST 双源/跨轮去重)
    std::deque<std::string> seen_fill_fifo_;  // FIFO 上限淘汰

    // 现实对账急停 (账本损坏也刹得住): 真实 USDC 较启动跌破 max_loss → KILL。用链上真实余额, 不信内部账本。
    // 真实 USDC 只随成交/持仓变 (挂单不锁), 故"跌幅 = 已实现亏 + 未平持仓成本" → 既抓亏损也抓失控累积。
    std::optional<double> usdc_start_;  // 启动时真实净值基线 (= 启动 USDC, 此时无持仓)
    // 账本 inv_pnl 急停的"本轮基线": 只对启动后新增的 inv_pnl 跌幅急停, 忽略历史/重启残留的幻象累积
    // (账本是脆弱来源 → 真正的安全是现实净值急停; 这个只作快速早警, 不让旧幻象反复误杀)。
    std::optional<double> inv_pnl_start_;
    double last_usdc_{0.0};             // 上次查到的真实 USDC (节流缓存)
    double last_pos_value_{0.0};        // 上次查到的链上持仓市值 (净值 = USDC + 此值)
    double last_usdc_check_{0.0};       // 上次查询单调时刻 (节流 ~15s)
    // 净值急停持续性计数: 链上持仓查询在刚买后会滞后(低估持仓)→ 净值瞬时假跌。要求连续 3 次 15s 采样都跌破
    // 才急停 (~45s) → 滤掉结算滞后的假跌, 真亏(持续)照样刹住。(在途记账对 BUY-NO 的 /data/trades 记法不可靠,
    // 已弃用; 现实信号 = 链上市值 + 持续性。)
    int equity_dd_count_{0};
    // 链上真实持仓 (token->size), ~15s 刷新。根因守卫: 持有未平仓位的池绝不再报价 → 不再被反复填成大孤立。
    std::map<std::string, double> last_chain_positions_;
    // 防churn熔断 (现实, 用原始成交): 某 token 60s 内被吃 > K 次 = 趋势反复扫我们 → KILL。账本无关。
    std::map<std::string, std::deque<double>> fill_times_;
};

}  // namespace pmm
