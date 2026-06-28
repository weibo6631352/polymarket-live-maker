// src/pmm/runner.cpp — 自主轮询循环实现 (port of pm_trader/runner.py LiveRunner)
#include "pmm/runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <sstream>
#include <thread>

#include "pmm/clob_submitter.hpp"
#include "pmm/env.hpp"
#include "pmm/maker_live.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/round.hpp"

namespace pmm {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_signal_stop{false};
void on_signal(int) { g_signal_stop.store(true); }

json selected_to_json(const portfolio::SelectedPool& s) {
    return {{"question", s.question},     {"condition_id", s.condition_id},
            {"token", s.token},           {"daily", s.daily},
            {"share", s.share},           {"min_size", s.min_size},
            {"size", s.size},             {"tick", s.tick},
            {"max_spread_c", s.max_spread_c}, {"half_spread_c", s.half_spread_c},
            {"committed_capital", s.committed_capital}, {"est_daily_reward", s.est_daily_reward},
            {"risk_adj_score", s.risk_adj_score}};
}

json pool_report_to_json(const rewards::PoolReport& p) {
    json j = {{"question", p.question},   {"condition_id", p.condition_id},
              {"token", p.token},         {"daily", p.daily},
              {"max_spread_c", p.max_spread_c}, {"min_size", p.min_size},
              {"tick", p.tick},           {"mid", p.mid},
              {"spread_c", p.spread_c},   {"inband_notional", p.inband_notional},
              {"share", p.share},         {"min_side_score", p.min_side_score},
              {"empty_band", p.empty_band}, {"reward_per_day", p.reward_per_day},
              {"gross_ann_pct", p.gross_ann_pct}, {"jump_verdict", p.jump_verdict}};
    if (p.max_jump_c) j["max_jump_c"] = *p.max_jump_c;
    if (p.daily_vol_c) j["daily_vol_c"] = *p.daily_vol_c;
    if (p.days_wiped) j["days_wiped"] = *p.days_wiped;
    return j;
}

double jget(const json& j, const char* key, double def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}

}  // namespace

LiveRunner::LiveRunner(RunnerConfig cfg, Engine* engine, rewards::RewardsClient* scanner,
                       ISubmitter* submitter)
    : cfg_(std::move(cfg)), engine_(engine), scanner_(scanner), submitter_(submitter) {
    if (cfg_.max_req_per_sec > 0.0) {  // >0 = 启用限流 (值已无意义: RateLimiter 用 per-endpoint 硬编码速率)
        rate_limiter_ = std::make_unique<RateLimiter>();  // per-endpoint 限额 (官方文档)
    }
    if (scanner_ == nullptr) {
        owned_scanner_ = std::make_unique<rewards::RewardsClient>(rate_limiter_.get());
        scanner_ = owned_scanner_.get();
    }
    // 选池由外部 curator (这个 LLM session) 完全动态驱动: bot 把候选发上 DDS (PoolEval),
    // curator 读着筛, 经 DDS CuratorCommand 把选中池白名单推回 → 这里热更 dyn_whitelist_, 不重启。
    // 有效白名单 = cfg_.pool_whitelist (静态种子, 可空) ∪ dyn_whitelist_ (curator 实时推)。
    publisher_->set_command_callback([this](const std::string& csv) {
        std::set<std::string> wl;
        std::stringstream ss(csv);
        std::string id;
        while (std::getline(ss, id, ',')) {
            const auto a = id.find_first_not_of(" \t");
            const auto b = id.find_last_not_of(" \t");
            if (a != std::string::npos) wl.insert(id.substr(a, b - a + 1));
        }
        const std::size_t n = wl.size();
        {
            std::lock_guard<std::mutex> lk(report_mu_);
            dyn_whitelist_ = std::move(wl);
        }
        std::fprintf(stderr, "curator: applied %zu pools via DDS\n", n);
    });
}

LiveRunner::~LiveRunner() {
    discovery_stop_.store(true);
    if (discovery_thread_.joinable()) discovery_thread_.join();
    for (auto& _rt : resync_threads_) if (_rt.joinable()) _rt.join();
    resync_threads_.clear();
}

rewards::RewardsClient& LiveRunner::scanner() { return *scanner_; }

// 墙钟毫秒 (遥测时间戳; mono_now 是单调时钟, 不能做 wall ts)。
static int64_t telemetry_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void LiveRunner::event(const std::string& kind, const json& fields) {
    if (events_) events_->write(kind, fields);
    // 全量遥测: 每个 event 走 DDS 的 LogEvent 兜底 topic。LogEvent IDL = {ts_ms,kind,condition_id,latency_ms,detail}:
    // 把整个 fields 串进 detail (一条不漏), condition_id 有则抽出便于按池过滤。富 topic 在各点另行显式 publish。
    if (publisher_)
        publisher_->publish(
            telemetry::topic::kLogEvent,
            {{"ts_ms", telemetry_wall_ms()},
             {"kind", kind},
             {"condition_id", fields.value("cond", fields.value("condition_id", std::string{}))},
             {"latency_ms", 0.0},
             {"detail", fields.dump()}});
}

nlohmann::json LiveRunner::locked_submit(const json& action) {
    std::lock_guard<std::mutex> lk(submitter_mu_);
    if (submitter_ == nullptr) return nullptr;
    return submitter_->submit(action);
}

bool LiveRunner::use_live_path() const {
    return (cfg_.live || cfg_.dry_live) && submitter_ != nullptr;
}

void LiveRunner::ensure_engine() {
    if (engine_ == nullptr) {
        owned_engine_ = std::make_unique<Engine>(cfg_.state_dir);
        engine_ = owned_engine_.get();
    }
    try {
        engine_->get_account();  // 新库抛 NotInitializedError
        engine_->sync_capital(cfg_.capital);  // 已有库: 把可用现金同步到当前 capital (改本金/充值后跟上)
    } catch (const NotInitializedError&) {
        engine_->init_account(cfg_.capital);  // 全新: cash = budget
    }
}

void LiveRunner::trip_kill(const std::string& reason) {
    if (!stop_.load()) {
        kill_reason_ = reason;
        stop_.store(true);
        std::fprintf(stderr, "KILL-SWITCH: %s — cancelling all + standing down\n", reason.c_str());
    }
}

std::vector<std::string> LiveRunner::held_tokens() {
    if (engine_ == nullptr) return {};
    std::vector<std::string> out;
    try {
        for (const auto& q : engine_->get_maker_quotes()) out.push_back(q.value("token_id", std::string{}));
    } catch (...) {
    }
    return out;
}

void LiveRunner::run() {
    std::fprintf(stderr, "%s\n", cfg_.banner().c_str());
    std::error_code ec;
    fs::create_directories(cfg_.state_dir, ec);
    if (cfg_.events_enabled && !events_) {
        events_ = std::make_unique<EventLog>(fs::path(cfg_.state_dir) / "events", cfg_.retention_days);
    }
    g_signal_stop.store(false);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    ensure_engine();
    engine_->set_maker_crossing_cost_c(cfg_.crossing_cost_c);
    engine_->set_micro_center(cfg_.micro_center, cfg_.micro_gate_c, cfg_.micro_beta);
    engine_->set_reward_calib(cfg_.reward_calib);  // 利润校准 κ (真实/毛估 ~0.237)
    engine_->set_jump_vol_weight(cfg_.jump_vol_weight);  // 跳变感知 bleed (毒池自动挂宽/净负)
    if (rate_limiter_) engine_->api().set_rate_limiter(rate_limiter_.get());

    // 配置/版本溯源遥测: 启动发一次 (哪套配置 + 哪个 commit 产出这批数据)。其余参数塞 extra_json。
    if (publisher_) {
        const json extra = {{"live", cfg_.live},
                            {"dry_live", cfg_.dry_live},
                            {"min_daily", cfg_.min_daily},
                            {"reward_calib", cfg_.reward_calib},
                            {"waterfill", cfg_.waterfill},
                            {"use_optimal_spread", cfg_.use_optimal_spread},
                            {"net_edge_gate", cfg_.net_edge_gate},
                            {"tail_k_sigma", cfg_.tail_k_sigma},
                            {"extreme_mid_margin", cfg_.extreme_mid_margin},
                            {"min_days_to_resolution", cfg_.min_days_to_resolution},
                            {"max_vol_mult", cfg_.max_vol_mult},
                            {"poll_seconds", cfg_.poll_seconds},
                            {"discovery_interval_s", cfg_.discovery_interval_s},
                            {"reeval_interval_s", cfg_.reeval_interval_s},
                            {"min_pool_reward", cfg_.min_pool_reward},
                            {"size_share_cap", cfg_.size_share_cap},
                            {"quality_floor_frac", cfg_.quality_floor_frac},
                            {"max_token_overlap", cfg_.max_token_overlap},
                            {"recenter_ticks", cfg_.recenter_ticks},
                            {"order_expiry_s", cfg_.order_expiry_s},
                            {"min_hold_s", cfg_.min_hold_s},
                            {"max_mid_vel_cps", cfg_.max_mid_vel_cps},
                            {"resync_workers", cfg_.resync_workers},
                            {"ws_enabled", cfg_.ws_enabled},
                            {"min_wallet_usdc", cfg_.min_wallet_usdc},
                            {"chop_aversion", cfg_.chop_aversion},
                            {"risk_tolerance_days", cfg_.risk_tolerance_days},
                            {"micro_center", cfg_.micro_center},
                            {"cooldown_rounds", cfg_.cooldown_rounds}};
#ifdef PMM_GIT_COMMIT
        const std::string git_commit = PMM_GIT_COMMIT;
#else
        const std::string git_commit;
#endif
        // 存为成员后周期重发 (主循环): 一次性首发会输给 DDS writer/reader 发现竞态 (发布在匹配完成前 → 丢);
        // 周期重发还让后加入的仪表盘客户端拿到当前配置。
        config_snapshot_ = {{"ts_ms", telemetry_wall_ms()},
                            {"git_commit", git_commit},
                            {"capital", cfg_.capital},
                            {"max_loss", cfg_.max_loss},
                            {"jump_vol_weight", cfg_.jump_vol_weight},
                            {"max_competitiveness", cfg_.max_competitiveness},
                            {"max_pool_frac", cfg_.max_pool_frac},
                            {"tail_budget", cfg_.tail_budget},
                            {"extra_json", extra.dump()}};
        publisher_->publish(telemetry::topic::kConfigSnapshot, config_snapshot_);
    }

    if (cfg_.live) {
        if (submitter_ == nullptr) {
            owned_submitter_ = std::make_unique<clob::ClobSubmitter>(rate_limiter_.get());
            submitter_ = owned_submitter_.get();
        }
        std::fprintf(stderr, "LIVE submitter armed — real orders will be placed\n");
        // 现实对账急停基线: 记下启动时真实 USDC。之后跌破 (基线 - max_loss) → KILL (账本损坏也刹得住)。
        if (auto bal = submitter_->usdc_balance(); bal && *bal > 0.0) {
            usdc_start_ = *bal;
            last_usdc_ = *bal;
            std::fprintf(stderr, "real-USDC kill armed: baseline $%.2f, floor $%.2f (drop > max_loss $%.0f)\n",
                         *bal, *bal - cfg_.max_loss, cfg_.max_loss);
        } else {
            std::fprintf(stderr, "WARN: could not read real USDC at startup — real-USDC kill DISABLED\n");
        }
    } else if (cfg_.dry_live && submitter_ == nullptr) {
        owned_submitter_ = std::make_unique<maker::DryRunSubmitter>();
        submitter_ = owned_submitter_.get();
        std::fprintf(stderr, "DRY-LIVE: rehearsing the live code path (orders logged, not sent)\n");
    }

    start_ws();
    rehydrate();
    reconcile_broker_orders();
    // 启动对账: 把账本两腿持仓校正到链上真实持仓 (修手动平仓/崩溃留下的幻象库存; 修双边记账根因之一)。
    if (use_live_path() && engine_ != nullptr) {
        const std::string funder = pmm::env::str("POLYMARKET_FUNDER");
        if (!funder.empty()) {
            const int fixed = engine_->reconcile_inventory(engine_->api().chain_positions(funder));
            if (fixed > 0)
                std::fprintf(stderr, "startup reconcile: corrected %d quote(s) to on-chain truth\n", fixed);
        }
    }
    // 成交游标预热: 把已存在的成交标记为"已见"。否则重启后 in-memory seen_fill_ids 被清空 → poll_fills 把
    // 旧成交(尤其手动平仓的大额 SELL)当新成交重复计数 → 库存跑飞成巨额幻象 → 假亏 → 假急停 (实测 NO=-1756)。
    if (use_live_path() && submitter_ != nullptr) {
        try {
            std::lock_guard<std::mutex> lk(submitter_mu_);
            const auto existing = submitter_->poll_fills();  // 推进 submitter 内部游标到最新
            for (const auto& f : existing) {
                const std::string id = f.value("id", std::string{});
                if (!id.empty()) {
                    seen_fill_ids_.insert(id);
                    seen_fill_fifo_.push_back(id);
                }
            }
            std::fprintf(stderr, "primed fill cursor: %zu existing trade(s) marked seen (no re-count)\n",
                         existing.size());
        } catch (...) {
        }
    }
    try {
        if (auto reason = kill_check()) {
            trip_kill(*reason);
            shutdown();
            return;
        }
        rediscover();
        reselect();
        start_discovery_thread();
        double last_reeval = mono_now();
        double last_stats = mono_now();
        long last_resync_count = 0;
        run_start_ = mono_now();
        while (!stop_.load()) {
            if (g_signal_stop.load()) trip_kill("signal");
            // 收到信号/kill 立刻退出: 不再多跑一次 poll_once + 整睡一个 poll 周期。
            // 否则关停被拖到 ~poll_seconds 之后才开始, 超过 systemd TimeoutStopSec 被 SIGKILL,
            // 真单残留在挂单簿上 (DRY 实测 SIGTERM→退出 ~40s, prod poll=60s 可逼近 ~120s)。
            if (stop_.load()) break;
            if (auto reason = kill_check()) {
                trip_kill(*reason);
                break;
            }
            const double now = mono_now();
            if (cfg_.stats_every_s > 0.0 && now - last_stats >= cfg_.stats_every_s) {
                const long rc = resync_count_.load(std::memory_order_relaxed);
                const double dt = now - last_stats;
                last_rps_ = dt > 0.0 ? static_cast<double>(rc - last_resync_count) / dt : 0.0;
                std::fprintf(stderr, "stats: /book resync %.1f req/s (%ld in %.0fs)\n", last_rps_,
                             rc - last_resync_count, dt);
                last_resync_count = rc;
                last_stats = now;
            }
            // 心跳遥测 (~2s): 运行态总览 (run/uptime/usdc/equity/quotes/盘口率)。
            if (publisher_ && now - last_heartbeat_ >= 2.0) {
                last_heartbeat_ = now;
                publisher_->publish(telemetry::topic::kHeartbeat,
                                    {{"ts_ms", telemetry_wall_ms()},
                                     {"run_state", "running"},
                                     {"uptime_s", now - run_start_},
                                     {"usdc", last_usdc_},
                                     {"equity", last_usdc_ + last_pos_value_},
                                     {"n_quotes", static_cast<int>(placed_.size())},
                                     {"n_pools_tracked", static_cast<int>(placed_.size())},
                                     {"book_resync_rps", last_rps_}});
            }
            // ConfigSnapshot 周期重发 (~30s): 防首发输给 DDS 发现竞态 + 让后加入的客户端拿到当前配置。
            if (publisher_ && !config_snapshot_.is_null() && now - last_config_emit_ >= 30.0) {
                last_config_emit_ = now;
                config_snapshot_["ts_ms"] = telemetry_wall_ms();
                publisher_->publish(telemetry::topic::kConfigSnapshot, config_snapshot_);
            }
            // PoolEval 全量重发 (~15s): 把缓存候选集 (report_.pools) 整体重发, 让任意时刻连上的外部 curator
            // (curate_read) 都能立即拿到当前全量候选 —— 不必赶在 discovery 突发窗口内 (发现间隔 600s)。纯发
            // 缓存, 不打 API。PoolEval 按 condition_id keyed → keyed reader 跨多次重发累积成"每池最新"全集。
            if (publisher_ && now - last_pooleval_emit_ >= 15.0) {
                last_pooleval_emit_ = now;
                std::vector<rewards::PoolReport> snap;
                {
                    std::lock_guard<std::mutex> lk(report_mu_);
                    snap = report_.pools;  // 拷出锁外发, 不长持 report_mu_
                }
                for (const auto& pr : snap) {
                    const double vm =
                        (pr.daily_vol_c && pr.max_spread_c > 0.0) ? *pr.daily_vol_c / pr.max_spread_c : 0.0;
                    publisher_->publish(telemetry::topic::kPoolEval,
                                        {{"ts_ms", telemetry_wall_ms()},
                                         {"condition_id", pr.condition_id},
                                         {"question", pr.question},
                                         {"competitiveness", pr.competitiveness},
                                         {"days_to_resolution", 0.0},
                                         {"mid", pr.mid},
                                         {"volume", pr.inband_notional},
                                         {"reward_rate_per_day", pr.daily},
                                         {"volume_24hr", pr.volume_24hr},
                                         {"jump_verdict", pr.jump_verdict},
                                         {"empty_band", pr.empty_band},
                                         {"vol_mult", vm},
                                         {"est_reward", pr.reward_per_day},
                                         {"net_per_day", pr.reward_per_day},
                                         {"safe_pass", true},
                                         {"comp_pass", true},
                                         {"mid_pass", true},
                                         {"net_pass", true},
                                         {"days_pass", true},
                                         {"reward_pass", true},
                                         {"selected", false},
                                         {"reject_reason", std::string{}}});
                }
            }
            if (now - last_reeval >= cfg_.reeval_interval_s) {
                tick_cooldowns();
                reevaluate_held();
                reselect();
                // 真实奖励感知: 查 PM 官方的真实 accrued + 实时占比, 记进 events (对照我们的毛估算)。
                if (use_live_path() && submitter_ != nullptr) {
                    try {
                        const json rw = submitter_->query_rewards();
                        // 实时校准监控: bot 毛估率 Σ(share×daily) vs 真实结算率 → 实测 κ (供调参/未来自校准)。
                        double gross_rate = 0.0;
                        for (const auto& q : engine_->get_maker_quotes()) {
                            const std::string tk = q.value("token_id", std::string{});
                            const std::string cd = q.value("market_condition_id", std::string{});
                            auto eit = share_ewma_.find(tk);
                            if (eit != share_ewma_.end() && placed_.count(cd))
                                gross_rate += eit->second * jget(placed_[cd], "daily", 0.0);
                        }
                        if (rw.is_object())
                            event("reward_real", {{"accrued_total", rw.value("accrued_total", 0.0)},
                                                  {"official_total", rw.value("official_total", 0.0)},
                                                  {"earning_markets", rw.value("earning_markets", 0)},
                                                  {"live_pct_markets", rw.value("live_pct_markets", 0)},
                                                  {"gross_rate_per_day", round_to(gross_rate, 2)}});
                    } catch (...) {
                    }
                }
                last_reeval = now;
            }
            poll_once();
            // 事件驱动: 等"盘口移动 (REST resync) / WS reflex"唤醒 → 立刻再跑一轮决策 (实时响应);
            // 或 poll_seconds 心跳超时 (兜底查成交/累计奖励)。CV 自动合并密集事件, 由 poll_once 时延限频。
            // 信号/stop 也唤醒 → SIGTERM 后立刻走关停。
            {
                std::unique_lock<std::mutex> lk(loop_mu_);
                loop_cv_.wait_for(lk, std::chrono::duration<double>(std::max(0.01, cfg_.poll_seconds)),
                                  [this] { return loop_wake_.load() || stop_.load() || g_signal_stop.load(); });
                loop_wake_.store(false);
            }
        }
    } catch (...) {
        // 任意异常都要走关停
    }
    shutdown();
}

void LiveRunner::rehydrate() {
    if (engine_ == nullptr) return;
    const double now = mono_now();
    int adopted = 0;
    for (const auto& q : engine_->get_maker_quotes()) {
        const std::string cond = q.value("market_condition_id", std::string{});
        if (cond.empty() || placed_.count(cond) != 0) continue;
        placed_[cond] = {{"condition_id", cond},
                         {"token", q.value("token_id", std::string{})},
                         {"question", q.value("market_slug", std::string{})},
                         {"half_spread_c", q.value("half_spread_c", 0.0)},
                         {"daily", q.value("daily_rate", 0.0)},
                         {"share", 0.0},
                         {"committed_capital", q.value("committed_capital", 0.0)},
                         {"est_daily_reward", 0.0}};
        placed_at_[cond] = now;
        ++adopted;
    }
    if (adopted > 0) std::fprintf(stderr, "rehydrated %d maker quote(s) from a prior run\n", adopted);
}

void LiveRunner::reconcile_broker_orders() {
    if (!cfg_.live || submitter_ == nullptr) return;
    std::set<std::string> held;
    for (const auto& [cond, m] : placed_) held.insert(m.value("token", std::string{}));
    std::vector<json> open_orders;
    try {
        open_orders = submitter_->list_open_orders();
    } catch (...) {
        return;
    }
    for (const auto& o : open_orders) {
        std::string token = o.value("asset_id", std::string{});
        if (token.empty()) token = o.value("token_id", std::string{});
        std::string oid = o.value("id", std::string{});
        if (oid.empty()) oid = o.value("orderID", o.value("order_id", std::string{}));
        if (!oid.empty() && held.count(token) == 0) {
            try {
                submitter_->cancel_order(oid);
            } catch (...) {
            }
        }
    }
}

void LiveRunner::start_ws() {
    if (!cfg_.ws_enabled) return;
    try {
        market_ch_ = std::make_unique<ws::MarketChannel>();
        market_ch_->set_price_callback([this](const std::string& t, double m) { on_ws_price(t, m); });
        market_ch_->start();
        engine_->set_book_source(market_ch_.get());
        start_book_resync();
    } catch (...) {
        market_ch_.reset();
    }
    if (cfg_.live && submitter_ != nullptr) {
        try {
            const json creds = submitter_->api_creds();
            if (creds.contains("apiKey")) {
                user_ch_ = std::make_unique<ws::UserChannel>(
                    ws::UserChannel::Creds{creds.value("apiKey", std::string{}),
                                           creds.value("secret", std::string{}),
                                           creds.value("passphrase", std::string{})},
                    submitter_->invert_side());
                user_ch_->start();
            }
        } catch (...) {
            user_ch_.reset();
        }
    }
}

void LiveRunner::start_book_resync() {
    if (!market_ch_ || cfg_.resync_workers <= 0 || !rate_limiter_) return;
    // 并发拉盘口: 单线程串行卡在 ~40ms 往返 = ~25/s, 够不到 /book 150/s 天花板。开 W 个 worker 并发,
    // 限流器统一节流到 150/s, 把权威盘口刷新率拉满 (16 池 → ~9Hz/池)。
    const int W = std::max(1, cfg_.resync_workers);
    for (int w = 0; w < W; ++w) {
        resync_threads_.emplace_back([this, w, W] {
            while (!discovery_stop_.load()) {
                std::vector<std::string> toks;
                {
                    std::lock_guard<std::mutex> lk(reflex_mu_);
                    for (const auto& [t, ref] : reflex_refs_) toks.push_back(t);
                }
                if (toks.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
                // worker w 负责 token w, w+W, w+2W... (切片不重叠); rate-limiter 把全体节流到 150/s。
                for (std::size_t i = static_cast<std::size_t>(w); i < toks.size();
                     i += static_cast<std::size_t>(W)) {
                    if (discovery_stop_.load()) break;
                    resync_once(toks[i]);
                }
            }
        });
    }
}

void LiveRunner::resync_once(const std::string& token) {
    try {
        const json book = scanner().book(token);
        if (!book.empty() && market_ch_) {
            market_ch_->apply_rest_snapshot(token, book);
            compute_and_store_signals(token);  // 在新鲜权威盘口上算预测信号 (高频, 零额外请求)
            resync_count_.fetch_add(1, std::memory_order_relaxed);
            publish_book_l2(token);  // 全档盘口遥测 (在权威盘口源头捕获全部档位, 非仅顶档)
        }
    } catch (...) {
    }
}

// OrderBookL2 全档快照遥测: 刚 resync 的权威盘口 → 全部 bid/ask 档 (价/量数组) + 派生 mid/inband_qmin。
// seq 全局单调 (每 token 子序列仍单调, 供丢更/连贯检测)。BEST_EFFORT, 永不回压交易循环。
void LiveRunner::publish_book_l2(const std::string& token) {
    if (!publisher_ || !market_ch_) return;
    auto ob = market_ch_->get_book(token);
    if (!ob) return;
    double bb = 0.0, ba = 0.0;
    json bids = json::array(), asks = json::array();
    for (const auto& l : ob->bids) {
        if (l.size > 0.0 && l.price > bb) bb = l.price;
        bids.push_back(json{{"price", l.price}, {"size", l.size}});
    }
    for (const auto& l : ob->asks) {
        if (l.size > 0.0 && (ba == 0.0 || l.price < ba)) ba = l.price;
        asks.push_back(json{{"price", l.price}, {"size", l.size}});
    }
    const double mid = (bb > 0.0 && ba > 0.0) ? (bb + ba) / 2.0 : 0.0;
    double v = 4.5;
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        auto it = signal_v_.find(token);
        if (it != signal_v_.end() && it->second > 0.0) v = it->second;
    }
    const double inband = (mid > 0.0) ? orderbook::book_inband_qmin(*ob, mid, v) : 0.0;
    publisher_->publish(telemetry::topic::kOrderBookL2,
                        {{"ts_ms", telemetry_wall_ms()},
                         {"condition_id", std::string{}},
                         {"token", token},
                         {"seq", book_seq_.fetch_add(1, std::memory_order_relaxed)},
                         {"source", "rest_resync"},
                         {"update_type", "snapshot"},
                         {"mid", mid},
                         {"inband_qmin", inband},
                         {"bids", bids},
                         {"asks", asks}});
}

double LiveRunner::mid_velocity(const std::string& token) {
    auto it = mid_hist_.find(token);
    if (it == mid_hist_.end() || it->second.size() < 2) return 0.0;
    const auto& h = it->second;
    if (h.back().first <= h.front().first) return 0.0;
    double moves = 0.0;
    for (std::size_t i = 1; i < h.size(); ++i) moves += std::abs(h[i].second - h[i - 1].second);
    return moves / (h.back().first - h.front().first) * 100.0;
}

void LiveRunner::sync_ws_subscriptions() {
    if (market_ch_) market_ch_->set_tokens(held_tokens());
    if (user_ch_) {
        std::vector<std::string> conds;
        for (const auto& [cond, m] : placed_) conds.push_back(cond);
        user_ch_->set_markets(conds);
    }
    refresh_reflex_refs();
}

void LiveRunner::refresh_reflex_refs() {
    std::map<std::string, std::pair<double, double>> refs;
    std::map<std::string, std::string> comp;
    std::map<std::string, double> vmap;
    if (engine_ != nullptr) {
        try {
            for (const auto& q : engine_->get_maker_quotes()) {
                const double tick = q.value("tick", 0.01) != 0.0 ? q.value("tick", 0.01) : 0.01;
                const double band = std::max(1, cfg_.recenter_ticks) * tick;
                const std::string tok = q.value("token_id", std::string{});
                refs[tok] = {q.value("last_mid", 0.0), band};
                comp[tok] = q.value("complement_token_id", std::string{});
                vmap[tok] = q.value("max_spread_c", 4.5);  // 信号带宽
            }
        } catch (...) {
            return;
        }
    }
    std::lock_guard<std::mutex> lk(reflex_mu_);
    reflex_refs_ = std::move(refs);
    reflex_complement_ = std::move(comp);
    signal_v_ = std::move(vmap);
}

// 在最新权威盘口 (REST resync 刚刷新) 上算预测信号, 存最新值 + 节流记标定日志。纯本地, 零额外请求。
void LiveRunner::compute_and_store_signals(const std::string& token) {
    if (market_ch_ == nullptr) return;
    auto ob = market_ch_->get_book(token);
    if (!ob || ob->bids.empty() || ob->asks.empty()) return;
    double v = 4.5;
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        auto it = signal_v_.find(token);
        if (it != signal_v_.end() && it->second > 0.0) v = it->second;
    }
    double bb = 0.0, ba = 0.0;
    for (const auto& l : ob->bids)
        if (l.size > 0.0 && l.price > bb) bb = l.price;
    for (const auto& l : ob->asks)
        if (l.size > 0.0 && (ba == 0.0 || l.price < ba)) ba = l.price;
    if (bb <= 0.0 || ba <= 0.0) return;
    const double mid = (bb + ba) / 2.0;
    // 事件驱动: REST 拉到的盘口越过 recenter 带 → 立刻唤醒主循环重挂 (实时响应, 不等 poll 心跳)。
    bool moved = false;
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        auto rit = reflex_refs_.find(token);
        if (rit != reflex_refs_.end() && rit->second.first > 0.0 &&
            std::abs(mid - rit->second.first) >= rit->second.second)
            moved = true;
    }
    if (moved) wake_loop();
    const orderbook::BookSignals sig = orderbook::compute_book_signals(*ob, mid, v);
    if (!sig.valid) return;
    // fade-on-imbalance: micro-price 大幅偏离 mid / 强订单流失衡 = 毒流来袭 → 抢在 mid 越带前双边撤。
    // 研究 (IEX/Cont): 毒流集中在瞬间, 静态挂着 = 知情流的出口; OBI 解释 ~65% 短时移动。防御性拉单, 非预测 skew。
    {
        const double micro_lead_c = std::abs(sig.micro_price - mid) * 100.0;
        const bool imbalanced =
            (cfg_.fade_micro_c > 0.0 && micro_lead_c >= cfg_.fade_micro_c) ||
            (cfg_.fade_obi > 0.0 && std::abs(sig.obi_band) >= cfg_.fade_obi);
        // 冷却: 撤后 fade_cooldown_s 秒内同 token 不再 fade。否则 fade→撤→wake 重挂→失衡还在→立刻再 fade
        // = 亚秒紧抖动, 报价永远歇不住、永远不成交 (实盘实测 25 次/2min)。冷却让重挂的单歇住、能被吃。
        if (imbalanced) {
            const double now_s = mono_now();
            bool do_fade = false;
            {
                std::lock_guard<std::mutex> lk(signal_mu_);
                auto it = fade_last_s_.find(token);
                if (it == fade_last_s_.end() || now_s - it->second >= cfg_.fade_cooldown_s) {
                    fade_last_s_[token] = now_s;
                    ++fade_since_poll_[token];  // 毒池检测计数
                    do_fade = true;
                }
            }
            if (do_fade) reflex_pull(token, "fade_imbalance", mid);
        }
    }
    // 标定日志: 每 token 节流 1s 记一次 (10Hz 全记会爆; 1s 给更细标定); mid/micro_price/obi + 下一周期 Δmid 供回归。
    const double now_m = mono_now();
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        auto it = signal_log_at_.find(token);
        if (it != signal_log_at_.end() && now_m - it->second < 1.0) return;  // 1s 节流 (更细标定数据)
        signal_log_at_[token] = now_m;
    }
    event("signal", {{"token", token},
                     {"mid", round_to(mid, 5)},
                     {"micro_price", round_to(sig.micro_price, 5)},
                     {"micro_lead_c", round_to((sig.micro_price - mid) * 100.0, 4)},
                     {"obi1", round_to(sig.obi1, 4)},
                     {"obi_band", round_to(sig.obi_band, 4)},
                     {"depth", round_to(sig.depth, 1)}});
}

// 拉单: 撤 token 双边报价 (mid-move reflex 与 imbalance fade 共用)。已占位/无活跃引用则跳过。
void LiveRunner::reflex_pull(const std::string& token, const std::string& reason, double mid) {
    std::string no_token;
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        if (reflex_refs_.count(token) == 0 || reflex_cancelled_.count(token) != 0) return;
        reflex_cancelled_.insert(token);  // 慢 I/O 前先占住 (并发再入直接跳过)
        auto cit = reflex_complement_.find(token);
        if (cit != reflex_complement_.end()) no_token = cit->second;
    }
    try {
        locked_submit({{"action", "CANCEL_ALL"}, {"token_id", token}});
        // YES 腿过时 → 互补 BUY-NO 腿同样过时, 一并撤掉。
        if (!no_token.empty()) locked_submit({{"action", "CANCEL_ALL"}, {"token_id", no_token}});
        event("reflex_cancel", {{"token", token}, {"mid", mid}, {"reason", reason}});
        wake_loop();  // 撤后立刻唤醒主循环在新 mid 重挂 (不留无报价空窗)
    } catch (...) {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        reflex_cancelled_.erase(token);
    }
}

void LiveRunner::on_ws_price(const std::string& token, double mid) {
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        auto it = reflex_refs_.find(token);
        if (it == reflex_refs_.end() || reflex_cancelled_.count(token) != 0) return;
        if (std::abs(mid - it->second.first) < it->second.second) return;  // 未越 recenter 带
    }
    reflex_pull(token, "mid_move", mid);  // mid 越带 → 拉 (滞后信号: mid 已动)
}

void LiveRunner::start_discovery_thread() {
    if (cfg_.discovery_interval_s <= 0.0) return;
    discovery_thread_ = std::thread([this] {
        while (!discovery_stop_.load()) {
            const double deadline = mono_now() + cfg_.discovery_interval_s;
            while (mono_now() < deadline) {
                if (discovery_stop_.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (discovery_stop_.load()) return;
            rediscover();
        }
    });
}

void LiveRunner::rediscover() {
    const double t0 = mono_now();
    try {
        rewards::ScanResult res = rewards::scan(scanner(), cfg_.min_daily, cfg_.scan_top, true,
                                                cfg_.min_days_to_resolution, cfg_.max_vol_mult,
                                                cfg_.reward_calib, cfg_.pool_whitelist);
        const int n_scanned = res.pools_scored;  // move 前抓计数 (供 DiscoveryScan 遥测)
        const int n_safe = res.safe_count;
        {
            std::lock_guard<std::mutex> lk(report_mu_);
            report_ = std::move(res);
            last_scan_ok_ = mono_now();
        }
        std::lock_guard<std::mutex> lk(report_mu_);
        std::fprintf(stderr, "discovery: %d safe of %d scored\n", report_.safe_count, report_.pools_scored);
        if (std::getenv("LM_LOG_CANDIDATES") != nullptr)  // 候选转储 (语义选池/调试): 可做价位的候选池
            for (const auto& pr : report_.pools)
                if (pr.mid > 0.15 && pr.mid < 0.85)
                    std::fprintf(stderr, "cand: %s mid=%.2f %s | %s\n", pr.condition_id.substr(0, 14).c_str(),
                                 pr.mid, pr.jump_verdict.c_str(), pr.question.substr(0, 52).c_str());
        // DiscoveryScan 遥测 (漏斗粗粒度): scan 总览计数 + 耗时。逐原因 drop 计数 scan 未单列 → 0 (TODO)。
        if (publisher_)
            publisher_->publish(telemetry::topic::kDiscoveryScan,
                                {{"ts_ms", telemetry_wall_ms()},
                                 {"n_scanned", n_scanned},
                                 {"n_safe", n_safe},
                                 {"n_dropped_jumpy", 0},
                                 {"n_dropped_depleted", 0},
                                 {"n_dropped_extreme", 0},
                                 {"n_dropped_comp", 0},
                                 {"n_selected", static_cast<int>(selected_.size())},
                                 {"scan_duration_ms", (mono_now() - t0) * 1000.0}});
    } catch (...) {
        // 保留上一份好 report; staleness guard 处理
    }
    // 选池决策不在这里做: dyn_whitelist_ 由外部 curator 经 DDS CuratorCommand 异步热更 (见 ctor 回调)。
}

bool LiveRunner::report_stale() {
    if (!last_scan_ok_) return true;
    return (mono_now() - *last_scan_ok_) > 2.0 * cfg_.discovery_interval_s;
}

void LiveRunner::reselect() {
    if (report_stale()) return;
    std::set<std::string> cd;
    for (const auto& [k, v] : cooldown_) {
        if (v > 0) cd.insert(k);
    }
    portfolio::SelectParams p;
    p.capital = cfg_.capital;
    p.half_spread_ticks = cfg_.half_spread_ticks;
    p.risk_tolerance_days = cfg_.risk_tolerance_days;
    p.chop_aversion = cfg_.chop_aversion;
    p.size_share_cap = cfg_.size_share_cap;
    p.loss_budget = cfg_.max_loss;
    p.quality_floor_frac = cfg_.quality_floor_frac;
    p.max_pool_frac = cfg_.max_pool_frac;
    p.max_token_overlap = cfg_.max_token_overlap;
    p.waterfill = cfg_.waterfill;        // 注水配资 (边际 κ×奖励/$ 均衡)
    p.reward_calib = cfg_.reward_calib;  // 利润校准 κ
    p.max_competitiveness = cfg_.max_competitiveness;  // 硬剔除新闻/毒池 (重新启用)
    p.extreme_mid_margin = cfg_.extreme_mid_margin;    // 剔除近极端价池 (逆选/趋势源)
    p.cooldown = cd;
    std::vector<rewards::PoolReport> scored_pools;
    {
        std::lock_guard<std::mutex> lk(report_mu_);
        // 有效白名单 = cfg_.pool_whitelist (静态人工覆盖, 始终通过) ∪ dyn_whitelist_ (LLM 批准, 动态)。
        // 都空 → 空白名单 → 数字滤网正常生效 (行为不变); 非空 → 只做名单内 (LLM 批准的池绕过脆滤网)。
        p.pool_whitelist = cfg_.pool_whitelist;            // 语义选池白名单 (非空只做名单内)
        for (const auto& c : dyn_whitelist_) p.pool_whitelist.insert(c);
        selected_ = portfolio::select_pools(report_, p);
        scored_pools = report_.pools;  // 复制供 PoolEval 遥测 (锁外发布, 不长持 report_mu_)
    }
    std::map<std::string, json> want;
    for (const auto& s : selected_) want[s.condition_id] = selected_to_json(s);

    // PoolEval 遥测: 每个评分候选发一条 (为什么选/拒) —— 全部指标 + 各滤网逐个 pass 结果。
    // net_per_day 在 scan 期未算 bleed → 用 reward_per_day 作代理; days_to_resolution 此处不在手 → 0。
    if (publisher_) {
        for (const auto& pr : scored_pools) {
            const bool safe_pass = pr.jump_verdict == "SAFE" || pr.jump_verdict.empty();
            const bool comp_pass = cfg_.max_competitiveness <= 0.0 || pr.competitiveness < 0.0 ||
                                   pr.competitiveness <= cfg_.max_competitiveness;
            const bool mid_pass = cfg_.extreme_mid_margin <= 0.0 ||
                                  (pr.mid >= cfg_.extreme_mid_margin && pr.mid <= 1.0 - cfg_.extreme_mid_margin);
            const bool reward_pass = pr.reward_per_day >= cfg_.min_pool_reward;
            const bool net_pass = pr.reward_per_day > 0.0;
            const bool is_sel = want.count(pr.condition_id) != 0;
            const double vol_mult =
                (pr.daily_vol_c && pr.max_spread_c > 0.0) ? *pr.daily_vol_c / pr.max_spread_c : 0.0;
            std::string reject;
            if (!is_sel) {
                if (!safe_pass) reject = "jumpy";
                else if (!comp_pass) reject = "competitive";
                else if (!mid_pass) reject = "extreme_mid";
                else if (pr.empty_band) reject = "empty_band";
                else if (!reward_pass) reject = "low_reward";
                else reject = "not_selected";
            }
            publisher_->publish(telemetry::topic::kPoolEval,
                                {{"ts_ms", telemetry_wall_ms()},
                                 {"condition_id", pr.condition_id},
                                 {"question", pr.question},
                                 {"competitiveness", pr.competitiveness},
                                 {"days_to_resolution", 0.0},
                                 {"mid", pr.mid},
                                 {"volume", pr.inband_notional},
                                 {"reward_rate_per_day", pr.daily},  // 原始日奖励率 (份额分子)
                                 {"volume_24hr", pr.volume_24hr},
                                 {"jump_verdict", pr.jump_verdict},
                                 {"empty_band", pr.empty_band},
                                 {"vol_mult", vol_mult},
                                 {"est_reward", pr.reward_per_day},
                                 {"net_per_day", pr.reward_per_day},
                                 {"safe_pass", safe_pass},
                                 {"comp_pass", comp_pass},
                                 {"mid_pass", mid_pass},
                                 {"net_pass", net_pass},
                                 {"days_pass", true},
                                 {"reward_pass", reward_pass},
                                 {"selected", is_sel},
                                 {"reject_reason", reject}});
        }
    }

    // DROP 不再在理想集中的持仓池
    if (engine_ != nullptr) {
        std::map<std::string, json> quotes_by_cond;
        for (const auto& q : engine_->get_maker_quotes()) quotes_by_cond[q.value("market_condition_id", std::string{})] = q;
        std::vector<std::string> conds;
        for (const auto& [cond, m] : placed_) conds.push_back(cond);
        for (const auto& cond : conds) {
            if (want.count(cond) == 0) {
                auto qit = quotes_by_cond.find(cond);
                if (qit != quotes_by_cond.end()) {
                    exit_held(cond, qit->second, "deselected");
                } else {
                    placed_.erase(cond);
                    placed_at_.erase(cond);
                }
            }
        }
    }
    // ADD 新选中 (不超预算)
    double committed = 0.0;
    for (const auto& [cond, m] : placed_) committed += m.value("committed_capital", 0.0);
    int n_want = static_cast<int>(want.size());
    int n_placed = 0, n_lowrew = 0, n_budget = 0, n_failed = 0, n_lownet = 0;
    for (const auto& [cond, s] : want) {
        if (placed_.count(cond) != 0) continue;
        // 白名单池绕过低奖励跳过: LLM 选这些是冲"价差/行为盈余"(宽基散户流), 非流动性奖励 —— 奖励小不代表不该做。
        if (cfg_.pool_whitelist.count(cond) == 0 &&
            s.value("est_daily_reward", 0.0) < cfg_.min_pool_reward) {
            ++n_lowrew;
            continue;
        }
        const double cap = s.value("committed_capital", 0.0);
        if (committed + cap > cfg_.capital + 1e-6) {
            ++n_budget;
            continue;
        }
        try {
            if (!place(cond, s)) {  // 净边际门跳过 (毒池, 未下单) → 不占选池槽
                ++n_lownet;
                continue;
            }
            committed += cap;
            placed_[cond] = s;
            placed_at_[cond] = mono_now();
            ++n_placed;
            event("place", {{"cond", cond}, {"daily", s.value("daily", 0.0)}, {"committed", cap}});
        } catch (const std::exception& e) {
            // 下单失败绝不能静默吞掉: 操作者会以为在做市, 其实一单没下。记 stderr + 事件。
            ++n_failed;
            std::fprintf(stderr, "place FAILED %s: %s\n", cond.c_str(), e.what());
            event("place_failed", {{"cond", cond}, {"committed", cap}, {"error", e.what()}});
        } catch (...) {
            ++n_failed;
            std::fprintf(stderr, "place FAILED %s: <unknown>\n", cond.c_str());
            event("place_failed", {{"cond", cond}, {"committed", cap}, {"error", "unknown"}});
        }
    }
    // 每轮选池小结: 选中多少 / 真下多少 / 各种跳过原因 — 否则 0 下单时无从判断卡在哪。
    std::fprintf(stderr,
                 "reselect: want=%d placed=%d (skip low_reward=%d over_budget=%d low_net=%d failed=%d)\n",
                 n_want, n_placed, n_lowrew, n_budget, n_lownet, n_failed);
    sync_ws_subscriptions();
}

bool LiveRunner::place(const std::string& cond, const json& pool) {
    double hs = pool.value("half_spread_c", 0.0);
    double sigma_c = 0.0;
    // QuoteDecision 遥测: 累积本次定价能读到的数学, 在每个出口 (净门跳过 / 尾部跳过 / 正常挂) 各发一条。
    double mid = 0.0, net_per_day = 0.0, reward_per_day = 0.0, bleed_per_day = 0.0, existing_qmin = 0.0;
    double share_w = pool.value("share", 0.0);
    double max_spread_c = pool.value("max_spread_c", 0.0);
    bool net_gate_pass = true, tail_capped = false;
    double tail_var = 0.0;
    double sz = pool.value("size", 0.0);
    const double min_size = pool.value("min_size", 0.0);
    auto emit_qd = [&](const char* status, const char* skip_reason) {
        if (!publisher_) return;
        const double own_qmin = (sz > 0.0 && hs > 0.0 && max_spread_c > 0.0)
                                    ? orderbook::maker_quote_score(sz, hs, max_spread_c)
                                    : 0.0;
        publisher_->publish(telemetry::topic::kQuoteDecision,
                            {{"ts_ms", telemetry_wall_ms()},
                             {"condition_id", cond},
                             {"question", pool.value("question", std::string{})},
                             {"token", pool.value("token", std::string{})},
                             {"side", "BUY-YES"},
                             {"book_seq", 0},
                             {"mid", mid},
                             {"max_spread_c", max_spread_c},
                             {"half_spread_c", hs},
                             {"sigma_c", sigma_c},
                             {"jump_sigma_c", sigma_c},  // engine σ 已是跳变感知 (jump_vol_weight>0); 同值
                             {"jump_anomaly", 0.0},
                             {"est_reward", reward_per_day},
                             {"bleed_per_day", bleed_per_day},
                             {"net_per_day", net_per_day},
                             {"existing_qmin", existing_qmin},
                             {"own_qmin", own_qmin},
                             {"share_w", share_w},
                             {"size", sz},
                             {"committed_capital", pool.value("committed_capital", 0.0)},
                             {"tail_var", tail_var},
                             {"net_gate_pass", net_gate_pass},
                             {"tail_capped", tail_capped},
                             {"status", status},
                             {"skip_reason", skip_reason}});
    };
    if (cfg_.use_optimal_spread) {
        try {
            const json rec = engine().suggest_maker_half_spread(cond, "yes", 0.0, cfg_.poll_seconds);
            mid = rec.value("mid", 0.0);
            net_per_day = rec.value("net_per_day", 0.0);
            reward_per_day = rec.value("reward_per_day", 0.0);
            bleed_per_day = rec.value("bleed_per_day", 0.0);
            existing_qmin = rec.value("existing_qmin", 0.0);
            share_w = rec.value("share", share_w);
            if (rec.contains("max_spread_c")) max_spread_c = rec.value("max_spread_c", max_spread_c);
            sigma_c = rec.value("sigma_c", 0.0);
            // 净边际门 (专家 #2): 跳变感知 bleed 后 net=reward-bleed ≤ 0 → 奖励被逆选吃光 → 不报价, 让毒池
            // 自然出局 (取代手调 comp/mid 启发式)。rec 缺字段时默认放行 (不误杀)。
            // 白名单池绕过净门: κ-bleed 模型保守/跳变盲, 会把宽基好池判净负误杀; LLM 判过 + fade 管逆选。
            // 有效白名单 = 静态 ∪ LLM 动态 (dyn_whitelist_ 与 report_ 同锁; place 不持 report_mu_, 短锁查)。
            bool whitelisted = cfg_.pool_whitelist.count(cond) != 0;
            if (!whitelisted) {
                std::lock_guard<std::mutex> lk(report_mu_);
                whitelisted = dyn_whitelist_.count(cond) != 0;
            }
            if (cfg_.net_edge_gate && !whitelisted && rec.value("net_per_day", 1.0) <= 0.0) {
                std::fprintf(stderr, "net-gate: %s net/day=%.3f <= 0 (bleed eats reward) — not quoting\n",
                             cond.substr(0, 10).c_str(), rec.value("net_per_day", 0.0));
                net_gate_pass = false;
                emit_qd("skipped", "net_gate");
                return false;
            }
            const double v = rec.value("half_spread_c", 0.0);
            if (v > 0.0) hs = v;
        } catch (...) {
        }
    }
    MakerQuoteOpts opts;
    opts.half_spread_cents = hs;
    // #4 尾部-VaR 限仓 (专家): 单次成交最坏损失 ≈ size × (k·σ)。$1k 无法分散尾部 → 限到 tail_budget。连 min_size
    // 都超预算 (毒池/高波动) → 不报价; 否则把 size 压进预算 (但不低于 min_size, 否则拿不到奖励门)。
    if (cfg_.tail_budget > 0.0 && sigma_c > 0.0) {
        const double worst_move = cfg_.tail_k_sigma * sigma_c / 100.0;  // 价格分数 (σ 是 cents)
        if (worst_move > 1e-9) {
            if (min_size * worst_move > cfg_.tail_budget) {
                std::fprintf(stderr, "tail-gate: %s min-tail $%.1f > budget $%.1f — not quoting\n",
                             cond.substr(0, 10).c_str(), min_size * worst_move, cfg_.tail_budget);
                tail_var = min_size * worst_move;
                emit_qd("skipped", "tail_budget");
                return false;
            }
            const double max_sz = cfg_.tail_budget / worst_move;
            if (sz > max_sz) {
                sz = std::max(min_size, max_sz);
                tail_capped = true;
            }
            tail_var = sz * worst_move;
        }
    }
    if (sz > 0.0) opts.size = sz;
    if (use_live_path()) {
        engine().place_maker_quote_live(cond, [this](const json& a) { return locked_submit(a); }, "yes", opts);
    } else {
        engine().place_maker_quote(cond, "yes", opts);
    }
    emit_qd("active", "");
    return true;
}

void LiveRunner::reevaluate_held() {
    if (!cfg_.reeval_enabled || placed_.empty() || engine_ == nullptr) return;
    std::map<std::string, json> quotes_by_cond;
    for (const auto& q : engine_->get_maker_quotes()) quotes_by_cond[q.value("market_condition_id", std::string{})] = q;
    const double now = mono_now();
    std::vector<std::pair<std::string, json>> worklist;
    std::vector<std::string> conds;
    for (const auto& [cond, m] : placed_) conds.push_back(cond);
    for (const auto& cond : conds) {
        auto qit = quotes_by_cond.find(cond);
        if (qit == quotes_by_cond.end()) {
            placed_.erase(cond);
            placed_at_.erase(cond);
            continue;
        }
        if (now - (placed_at_.count(cond) ? placed_at_[cond] : 0.0) < cfg_.min_hold_s) continue;
        worklist.push_back({cond, qit->second});
    }
    if (worklist.empty()) return;

    // 并发 rescore (只读 I/O); 退出决策串行。
    std::vector<std::optional<json>> fresh(worklist.size());
    const int workers = std::min<int>(REEVAL_WORKERS, static_cast<int>(worklist.size()));
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= worklist.size()) break;
            const json& q = worklist[i].second;
            fresh[i] = rescore(worklist[i].first, q.value("token_id", std::string{}),
                               q.value("size", 0.0), q.value("half_spread_c", 0.0));
        }
    };
    std::vector<std::thread> ths;
    for (int w = 0; w < workers; ++w) ths.emplace_back(worker);
    for (auto& t : ths) t.join();

    for (std::size_t i = 0; i < worklist.size(); ++i) {
        const auto& [cond, q] = worklist[i];
        const std::string tok = q.value("token_id", std::string{});
        // fade 率毒池退出: 池连续 N 个 poll 都在 fade (顶着冷却上限) = 持续性失衡/毒流, 挂着也只被单边吃
        // → 退出 + 冷却 (kCooldownReasons 含 fade_toxic), 别死磕。实盘实测 Beşiktaş 即此型。白名单不豁免此退出。
        if (cfg_.fade_toxic_streak > 0) {
            int faded = 0;
            {
                std::lock_guard<std::mutex> lk(signal_mu_);
                if (auto fit = fade_since_poll_.find(tok); fit != fade_since_poll_.end()) {
                    faded = fit->second;
                    fit->second = 0;
                }
            }
            if (faded > 0) ++fade_streak_[cond]; else fade_streak_[cond] = 0;
            if (fade_streak_[cond] >= cfg_.fade_toxic_streak) {
                fade_streak_[cond] = 0;
                exit_held(cond, q, "fade_toxic");
                continue;
            }
        }
        // 真实份额踢死池 (独立于 rescore, 即使 rescore 失败也能踢): bot 实测在带份额 EWMA × daily < 门槛
        // → 该池已被竞争稀释成 ~$0 (且大单挂在那易被整个扫掉 → 方向性亏损, 实测吃过亏), 退出腾资金给好池。
        if (auto eit = share_ewma_.find(tok); eit != share_ewma_.end()) {
            double daily0 = (fresh[i] && fresh[i]->contains("daily")) ? jget(*fresh[i], "daily", 0.0) : 0.0;
            if (daily0 <= 0.0 && placed_.count(cond)) daily0 = jget(placed_[cond], "daily", 0.0);
            if (daily0 > 0.0 && eit->second * daily0 < cfg_.min_pool_reward) {
                exit_held(cond, q, "share_collapsed");
                continue;
            }
        }
        if (!fresh[i]) continue;
        auto reason = degrade_reason(*fresh[i]);
        if (!reason && cfg_.max_mid_vel_cps > 0.0) {
            const double vel = mid_velocity(tok);
            if (vel > cfg_.max_mid_vel_cps) reason = "fast_book";
        }
        if (reason) exit_held(cond, q, *reason);
    }
}

std::optional<json> LiveRunner::rescore(const std::string& cond, const std::string& token, double own_size,
                                        double own_hs) {
    std::optional<rewards::RewardConfig> cfg;
    try {
        cfg = engine().api().get_reward_config(cond);
    } catch (...) {
        return std::nullopt;
    }
    if (!cfg || cfg->daily <= 0.0) return json{{"daily", 0.0}};  // 离开计划 → 退出
    json book;
    std::vector<orderbook::PricePoint> history;
    try {
        book = scanner().book(token);
        history = scanner().prices_history(token);
    } catch (...) {
        return std::nullopt;
    }
    auto scored = rewards::score_pool(*cfg, book, history);
    if (!scored) return json{{"one_sided", true}};
    json fresh = pool_report_to_json(*scored);
    const double c = scored->max_spread_c;
    if (own_size > 0.0 && own_hs > 0.0 && c > 0.0) {
        const double own_q = own_size * ((c - own_hs) / c) * ((c - own_hs) / c);
        const double competitor = std::max(0.0, scored->min_side_score - own_q);
        const double share = rewards::reward_share(cfg->min_size, cfg->tick, c, competitor);
        fresh["share"] = round_to(share, 4);
        fresh["reward_per_day"] = round_to(share * cfg->daily, 2);
    }
    return fresh;
}

std::optional<std::string> LiveRunner::degrade_reason(const json& fresh) {
    if (fresh.value("one_sided", false)) return "one_sided";
    const double daily = jget(fresh, "daily", 0.0);
    if (daily <= 0.0) return "rewards_ended";
    if (daily < cfg_.min_daily) return "daily_cut";
    const std::string jv = fresh.value("jump_verdict", std::string{});
    if (jv == "WATCH" || jv == "KILL") return "jump_risk_rose";
    if (fresh.value("empty_band", false)) return "empty_band";
    double reward;
    auto it = fresh.find("reward_per_day");
    if (it != fresh.end() && it->is_number()) {
        reward = it->get<double>();
    } else {
        reward = jget(fresh, "share", 1.0) * daily;
    }
    if (reward < cfg_.min_pool_reward) return "reward_collapsed";
    return std::nullopt;
}

void LiveRunner::exit_held(const std::string& cond, const json& quote, const std::string& reason) {
    // 白名单池: LLM 选它是冲价差/行为盈余, 不因"奖励份额塌缩/跳动"这类启发式退出 (否则会 place→exit 抖动)。
    // 功能性退出 (empty_band/one_sided/fast_book) 仍生效 —— 盘口真不可做时照退。
    static const std::set<std::string> kRewardOrJumpExits = {
        "reward_collapsed", "daily_cut", "share_collapsed", "jump_risk_rose"};
    if (cfg_.pool_whitelist.count(cond) != 0 && kRewardOrJumpExits.count(reason) != 0) {
        return;
    }
    static const std::set<std::string> kCooldownReasons = {
        "jump_risk_rose", "empty_band",   "reward_collapsed", "daily_cut",
        "one_sided",      "fast_book",    "share_collapsed",  "fade_toxic"};
    const std::string token = quote.value("token_id", std::string{});
    const std::string no_token = quote.value("complement_token_id", std::string{});
    const int qid = quote.value("id", 0);
    const double inv = quote.value("inventory", 0.0);
    if (use_live_path() && submitter_ != nullptr) {
        locked_submit({{"action", "CANCEL_ALL"}, {"token_id", token}});
        if (!no_token.empty()) locked_submit({{"action", "CANCEL_ALL"}, {"token_id", no_token}});  // BUY-NO 腿
        if (cfg_.live) {
            // FLATTEN 真实库存 (持有的是 YES 份额, 卖出平仓)
            if (std::abs(inv) >= 1e-9) {
                const std::string side = inv > 0 ? "SELL" : "BUY";
                locked_submit({{"action", "FLATTEN"}, {"token_id", token}, {"side", side}, {"size", std::abs(inv)}});
            }
        }
    }
    engine().cancel_maker_quote(qid);
    placed_.erase(cond);
    placed_at_.erase(cond);
    if (kCooldownReasons.count(reason) != 0) cooldown_[cond] = cfg_.cooldown_rounds;
    event("exit", {{"cond", cond}, {"reason", reason}, {"inventory", inv}});
}

void LiveRunner::tick_cooldowns() {
    for (auto it = cooldown_.begin(); it != cooldown_.end();) {
        it->second -= 1;
        if (it->second <= 0) {
            it = cooldown_.erase(it);
        } else {
            ++it;
        }
    }
}

FillsByToken LiveRunner::poll_fills() {
    std::vector<json> fills;
    try {
        // REST /data/trades (按 funder maker_address) 是权威成交源 — 每轮都查。
        if (submitter_ != nullptr) {
            std::lock_guard<std::mutex> lk(submitter_mu_);
            fills = submitter_->poll_fills();
        }
        // WS user channel 低延迟补充, 合并; runner 级按 id 去重防双源/跨轮重复计数。
        if (user_ch_) {
            auto ws = user_ch_->poll_fills();
            fills.insert(fills.end(), ws.begin(), ws.end());
        }
    } catch (...) {
        return {};
    }
    FillsByToken out;
    for (const auto& f : fills) {
        const std::string id = f.value("id", std::string{});
        if (!id.empty()) {
            if (seen_fill_ids_.count(id) != 0) continue;  // 已计过这笔成交
            seen_fill_ids_.insert(id);
            seen_fill_fifo_.push_back(id);
            if (seen_fill_fifo_.size() > 5000) {
                seen_fill_ids_.erase(seen_fill_fifo_.front());
                seen_fill_fifo_.pop_front();
            }
        }
        RealFill rf;
        rf.side = f.value("side", std::string{});
        rf.size = f.value("size", 0.0);
        rf.price = f.value("price", 0.0);
        const std::string tok = f.value("token_id", std::string{});
        out[tok].push_back(rf);
        // FillContext 遥测: 每笔新成交的逆选画面 (成交时盘口在手)。成交前 mid / bleed / inventory_after / 腿
        // 不在此处计算 (在 engine accrual 内) → 置 0/""; book_bid/ask/mid_after 从当前 WS 盘口取 (若有)。
        if (publisher_ && !tok.empty()) {
            double bb = 0.0, ba = 0.0;
            if (market_ch_) {
                if (auto ob = market_ch_->get_book(tok)) {
                    for (const auto& l : ob->bids)
                        if (l.size > 0.0 && l.price > bb) bb = l.price;
                    for (const auto& l : ob->asks)
                        if (l.size > 0.0 && (ba == 0.0 || l.price < ba)) ba = l.price;
                }
            }
            const double mid_after = (bb > 0.0 && ba > 0.0) ? (bb + ba) / 2.0 : 0.0;
            publisher_->publish(telemetry::topic::kFillContext,
                                {{"ts_ms", telemetry_wall_ms()},
                                 {"condition_id", std::string{}},
                                 {"question", std::string{}},
                                 {"token", tok},
                                 {"side", rf.side},
                                 {"size", rf.size},
                                 {"price", rf.price},
                                 {"leg", std::string{}},
                                 {"mid_before", 0.0},
                                 {"mid_after", mid_after},
                                 {"bleed", 0.0},
                                 {"book_bid", bb},
                                 {"book_ask", ba},
                                 {"book_seq", 0},
                                 {"inventory_after", 0.0},
                                 {"reconciled", false}});
        }
        // 防churn熔断: 这个 token 60s 内被吃太多次 = 被趋势反复扫 (逆选 churn) → KILL (现实, 不依赖账本)。
        if (!tok.empty()) {
            const double tn = mono_now();
            auto& ft = fill_times_[tok];
            ft.push_back(tn);
            while (!ft.empty() && tn - ft.front() > 60.0) ft.pop_front();
            if (ft.size() >= 8) {
                trip_kill("churn: " + std::to_string(ft.size()) + " maker-fills/60s on token " + tok.substr(0, 12));
            }
        }
    }
    return out;
}

std::vector<json> LiveRunner::poll_once() {
    std::vector<json> rows;
    if (use_live_path()) {
        const FillsByToken fills = poll_fills();
        std::set<std::string> forced;
        {
            std::lock_guard<std::mutex> lk(reflex_mu_);
            forced = std::move(reflex_cancelled_);
            reflex_cancelled_.clear();
        }
        if (cfg_.order_expiry_s > 0.0) {  // GTD dead-man refresh
            const double interval = cfg_.order_expiry_s / 2.0;
            const double nowm = mono_now();
            std::set<std::string> held;
            for (const auto& [cond, m] : placed_) {
                const std::string tok = m.value("token", std::string{});
                if (!tok.empty()) held.insert(tok);
            }
            for (const auto& tok : held) {
                auto it = refresh_at_.find(tok);
                if (it == refresh_at_.end()) {
                    refresh_at_[tok] = nowm;
                } else if (nowm - it->second >= interval) {
                    forced.insert(tok);
                    refresh_at_[tok] = nowm;
                }
            }
            for (auto it = refresh_at_.begin(); it != refresh_at_.end();) {
                it = (held.count(it->first) == 0) ? refresh_at_.erase(it) : std::next(it);
            }
        }
        rows = engine().accrue_maker_rewards_live([this](const json& a) { return locked_submit(a); }, fills,
                                                  std::nullopt, cfg_.recenter_ticks, &forced);
    } else {
        rows = engine().accrue_maker_rewards();
    }
    const double now_m = mono_now();
    for (const auto& row : rows) {
        const json q = row.value("quote", json::object());
        const std::string tok = q.value("token_id", std::string{});
        if (auto mit = row.find("mid"); mit != row.end() && mit->is_number() && mit->get<double>() > 0.0 && !tok.empty()) {
            auto& h = mid_hist_[tok];
            h.push_back({now_m, mit->get<double>()});
            while (h.size() > 120) h.pop_front();
        }
        // 实测在带份额 EWMA (复评据此踢被稀释成 $0 的死池)。α=0.2 → 反映近 ~5-10 次 poll。
        if (auto sit = row.find("share"); sit != row.end() && sit->is_number() && !tok.empty()) {
            const double s = sit->get<double>();
            auto eit = share_ewma_.find(tok);
            share_ewma_[tok] = (eit == share_ewma_.end()) ? s : 0.8 * eit->second + 0.2 * s;
        }
        // per-pool "poll" 事件 (节流; fills/reconcile/exit 总记)。对齐 Python 的 review 日志。
        const bool notable = row.contains("fills_applied") || row.contains("reconciled") || row.contains("exit_failed");
        const bool due = poll_evt_at_.count(tok) == 0 || now_m - poll_evt_at_[tok] >= cfg_.event_poll_every_s;
        if (events_ && (notable || due)) {
            poll_evt_at_[tok] = now_m;
            json ev;
            ev["cond"] = q.value("market_condition_id", std::string{});
            ev["token"] = tok;
            auto put = [&](const char* evk, const char* rowk) {
                ev[evk] = row.contains(rowk) ? row[rowk] : json(nullptr);
            };
            put("mid", "mid");
            put("reward", "reward");
            put("share", "share");
            put("inventory", "inventory");
            put("inv_pnl_delta", "inventory_pnl_delta");
            put("fills", "fills_applied");
            put("reconciled", "reconciled");
            put("exit_failed", "exit_failed");
            event("poll", ev);
        }
        if (row.contains("exit_failed")) continue;
        if (auto rit = row.find("reconciled"); rit != row.end() && rit->is_string()) {
            const std::string cond = q.value("market_condition_id", std::string{});
            placed_.erase(cond);
            placed_at_.erase(cond);
            if (rit->get<std::string>() != "rewards_ended") cooldown_[cond] = cfg_.cooldown_rounds;
        }
    }
    refresh_reflex_refs();
    return rows;
}

std::optional<std::string> LiveRunner::kill_check() {
    if (fs::exists(cfg_.kill_file) || fs::exists(fs::path(cfg_.state_dir) / cfg_.kill_file)) {
        return "kill-file";
    }
    if (engine_ != nullptr) {
        double inv_pnl = 0.0;
        try {
            inv_pnl = engine_->get_maker_summary().value("inventory_pnl", 0.0);
        } catch (...) {
        }
        // 本轮基线: 首次记下启动时的累积 inv_pnl, 之后只对"较启动新增的跌幅"急停 → 重启/历史的残留幻象
        // 不会反复误杀。真正兜底是现实净值急停 (链上, 不信账本); 这个账本急停仅作本轮快速早警。
        if (!inv_pnl_start_) inv_pnl_start_ = inv_pnl;
        if (inv_pnl - *inv_pnl_start_ <= -cfg_.max_loss) {
            return "max-loss (this-run maker inv P&L drop $" + std::to_string(inv_pnl - *inv_pnl_start_) + ")";
        }
    }
    // 现实对账急停 (最关键的安全网, 账本损坏也刹得住): 真实 USDC 较启动基线跌破 max_loss → KILL。
    // 真实 USDC 只随成交/持仓变 → 跌幅同时抓"已实现亏损"与"失控累积未平持仓"。节流 ~15s 查一次。
    if (cfg_.live && usdc_start_ && submitter_ != nullptr) {
        const double now = mono_now();
        if (now - last_usdc_check_ >= 15.0) {
            last_usdc_check_ = now;
            if (auto bal = submitter_->usdc_balance(); bal && *bal >= 0.0) last_usdc_ = *bal;
            // 真实净值 = USDC + 链上持仓市值: 持仓不算亏 (只有逆向跌价才算) → 不会因正常成交持仓误杀。
            if (engine_ != nullptr) {
                const std::string funder = pmm::env::str("POLYMARKET_FUNDER");
                last_pos_value_ = engine_->api().chain_position_value(funder);
                last_chain_positions_ = engine_->api().chain_positions(funder);  // 根因守卫用 (持仓的池不再报价)
                // 每周期对账: 把活跃 quote 两腿持仓校正到链上真实。修 fill 误腿: BUY-NO 经 MINT 被 /data/trades
                // 记在 YES token 上 → poll_fills 记成 YES 腿 → flatten 去平 YES(没持有)→ 永久孤立 (实测 Messi)。
                // 校正后 flatten 对的是链上真实持有的腿 → 能平掉。链上为准, 与 fill 怎么记无关。
                engine_->reconcile_inventory(last_chain_positions_);
                engine_->set_chain_positions(last_chain_positions_);  // 根因守卫用链上真实持仓
                // 孤立清扫: 链上持有、但无活跃 quote 在管的仓 (pool 退出/降级时平仓没完成留下) → 主动平掉。
                // reconcile 只校正活跃 quote, 退出后的孤立它只告警不平 → 这里兜底 (实测 168 NO 退出孤立卡 5min)。
                std::set<std::string> active_toks;
                for (const auto& q : engine_->get_maker_quotes()) {
                    const std::string ty = q.value("token_id", std::string{});
                    const std::string tn = q.value("complement_token_id", std::string{});
                    if (!ty.empty()) active_toks.insert(ty);
                    if (!tn.empty()) active_toks.insert(tn);
                }
                for (const auto& [tok, sz] : last_chain_positions_) {
                    if (std::abs(sz) < 1.0 || active_toks.count(tok) != 0) continue;
                    std::fprintf(stderr, "orphan-sweep: flattening untracked %.0f on %s\n", sz,
                                 tok.substr(0, 12).c_str());
                    try {
                        locked_submit({{"action", "FLATTEN"},
                                       {"token_id", tok},
                                       {"side", sz > 0.0 ? "SELL" : "BUY"},
                                       {"size", std::abs(sz)}});
                        event("orphan_sweep", {{"token", tok}, {"size", sz}});
                    } catch (...) {
                    }
                }
            }
            // 净值急停持续性: 链上持仓查询刚买后滞后→净值瞬时假跌。连续 3 次 15s 采样(~45s)都跌破才急停 →
            // 滤掉结算滞后的假跌, 真亏(持续)照样刹住。不用在途 net_cost: /data/trades 把 BUY-NO 记成 SELL-YES,
            // 在途净成本变负 → 净值假跌 → 假急停 (实测 5min 内停机)。链上市值 + 持续性才是稳的现实信号。
            const double eq = last_usdc_ + last_pos_value_;
            if (last_usdc_ > 0.0 && eq < *usdc_start_ - cfg_.max_loss)
                equity_dd_count_++;
            else
                equity_dd_count_ = 0;
            publish_risk_equity();  // live: 用刚刷新的链上真实净值/持仓发 Equity/Position/Risk 遥测 (15s)
        }
        if (equity_dd_count_ >= 3) {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "real-equity-drawdown x%d (now $%.2f [usdc %.2f+pos %.2f] < baseline $%.2f - maxloss $%.0f)",
                          equity_dd_count_, last_usdc_ + last_pos_value_, last_usdc_, last_pos_value_, *usdc_start_,
                          cfg_.max_loss);
            return std::string(buf);
        }
    }
    if (cfg_.live && cfg_.min_wallet_usdc > 0.0 && submitter_ != nullptr) {
        const auto bal = submitter_->usdc_balance();
        if (bal && *bal < cfg_.min_wallet_usdc) {
            return "wallet-floor (real USDC $" + std::to_string(*bal) + ")";
        }
    }
    // DRY-LIVE 遥测: live 的 15s 链上刷新块此时不跑 (cfg_.live=false), 但仪表盘要持续有净值/持仓/风险态 →
    // 用账本派生数据 (engine summary + 模拟两腿库存) 按 15s 节流发。绝不影响交易 (纯发布)。
    if (publisher_ && !cfg_.live) {
        const double nowt = mono_now();
        if (nowt - last_tel_check_ >= 15.0) {
            last_tel_check_ = nowt;
            publish_risk_equity();
        }
    }
    return std::nullopt;
}

// EquitySnapshot + Position(每非零腿) + RiskState 遥测。live 用刚刷新的链上真实数据 (last_usdc_/
// last_pos_value_/last_chain_positions_); dry-live 这些为 0/空 → 用 engine 账本 summary + 模拟两腿库存。
void LiveRunner::publish_risk_equity() {
    if (!publisher_ || engine_ == nullptr) return;
    json summary;
    try {
        summary = engine_->get_maker_summary();
    } catch (...) {
        summary = json::object();
    }
    const double reward_accrued = summary.value("reward_income", 0.0);
    const double realized_bleed = summary.value("adverse_bleed", 0.0);
    const double day_pnl = summary.value("net_maker_pnl", 0.0);
    const double committed = summary.value("committed_capital", 0.0);
    const double equity = last_usdc_ + last_pos_value_;
    const int64_t ts = telemetry_wall_ms();

    publisher_->publish(telemetry::topic::kEquitySnapshot,
                        {{"ts_ms", ts},
                         {"usdc", last_usdc_},
                         {"position_value", last_pos_value_},
                         {"equity", equity},
                         {"equity_dd_count", equity_dd_count_},
                         {"day_pnl", day_pnl},
                         {"realized_bleed", realized_bleed},
                         {"reward_accrued", reward_accrued}});

    // Position: live → 链上真实持仓 (token->size); dry-live (链上未刷新) → engine 账本两腿模拟库存。
    if (!last_chain_positions_.empty()) {
        for (const auto& [tok, psz] : last_chain_positions_) {
            if (std::abs(psz) < 1e-9) continue;
            publisher_->publish(telemetry::topic::kPosition, {{"ts_ms", ts},
                                                              {"condition_id", std::string{}},
                                                              {"token", tok},
                                                              {"outcome", std::string{}},
                                                              {"size", psz},
                                                              {"value", 0.0}});
        }
    } else {
        try {
            for (const auto& q : engine_->get_maker_quotes()) {
                const double qmid = q.value("last_mid", 0.0);
                const double inv = q.value("inventory", 0.0);
                const double cinv = q.value("complement_inventory", 0.0);
                const std::string qcond = q.value("market_condition_id", std::string{});
                if (std::abs(inv) >= 1e-9)
                    publisher_->publish(telemetry::topic::kPosition,
                                        {{"ts_ms", ts},
                                         {"condition_id", qcond},
                                         {"token", q.value("token_id", std::string{})},
                                         {"outcome", "yes"},
                                         {"size", inv},
                                         {"value", inv * qmid}});
                if (std::abs(cinv) >= 1e-9)
                    publisher_->publish(telemetry::topic::kPosition,
                                        {{"ts_ms", ts},
                                         {"condition_id", qcond},
                                         {"token", q.value("complement_token_id", std::string{})},
                                         {"outcome", "no"},
                                         {"size", cinv},
                                         {"value", cinv * (1.0 - qmid)}});
            }
        } catch (...) {
        }
    }

    // RiskState: 各刹车/预算的连续态 (best-effort)。
    long churn = 0;
    for (const auto& [tok, dq] : fill_times_) churn = std::max(churn, static_cast<long>(dq.size()));
    long chain_nonzero = 0;
    for (const auto& [tok, psz] : last_chain_positions_)
        if (std::abs(psz) >= 1.0) ++chain_nonzero;
    const bool kill_armed = cfg_.live && usdc_start_.has_value();
    const double drawdown = (cfg_.live && usdc_start_) ? std::max(0.0, *usdc_start_ - equity)
                                                       : std::max(0.0, -day_pnl);
    publisher_->publish(telemetry::topic::kRiskState,
                        {{"ts_ms", ts},
                         {"kill_armed", kill_armed},
                         {"equity_dd_count", equity_dd_count_},
                         {"churn_count", churn},
                         {"chain_flat_guard", chain_nonzero > 0},
                         {"capital_deployed", committed},
                         {"tail_budget_used", 0.0},
                         {"drawdown", drawdown}});
}

void LiveRunner::shutdown() {
    discovery_stop_.store(true);
    if (discovery_thread_.joinable()) discovery_thread_.join();
    for (auto& _rt : resync_threads_) if (_rt.joinable()) _rt.join();
    resync_threads_.clear();
    if (market_ch_) market_ch_->stop();
    if (user_ch_) user_ch_->stop();
    if (events_) events_->close();
    std::fprintf(stderr, "shutdown (%s): cancelling all maker quotes\n",
                 kill_reason_.empty() ? "stop" : kill_reason_.c_str());
    if (engine_ != nullptr) {
        try {
            for (const auto& q : engine_->get_maker_quotes()) {
                const std::string token = q.value("token_id", std::string{});
                const std::string no_token = q.value("complement_token_id", std::string{});
                if (use_live_path()) {
                    locked_submit({{"action", "CANCEL_ALL"}, {"token_id", token}});
                    if (!no_token.empty())
                        locked_submit({{"action", "CANCEL_ALL"}, {"token_id", no_token}});  // BUY-NO 腿
                    if (cfg_.live) {
                        const double inv = q.value("inventory", 0.0);
                        if (std::abs(inv) >= 1e-9) {
                            const std::string side = inv > 0 ? "SELL" : "BUY";
                            locked_submit({{"action", "FLATTEN"}, {"token_id", token}, {"side", side}, {"size", std::abs(inv)}});
                        }
                    }
                }
                engine_->cancel_maker_quote(q.value("id", 0));
            }
            // 安全收尾第 1 步 —— 先撤掉 CLOB 上所有挂单 (无论 tracked 与否), 这样平仓期间不会再有新成交。
            // 防撤单竞态/漏网单留在盘口, 软件关了之后被成交造成失控亏损。
            if (use_live_path() && submitter_ != nullptr) {
                try {
                    const std::vector<json> open = submitter_->list_open_orders();
                    int swept = 0;
                    for (const auto& o : open) {
                        std::string oid = o.value("id", std::string{});
                        if (oid.empty()) oid = o.value("orderID", o.value("order_id", std::string{}));
                        if (!oid.empty()) {
                            submitter_->cancel_order(oid);
                            ++swept;
                        }
                    }
                    std::fprintf(stderr, "shutdown: swept %d residual open order(s) off the book\n", swept);
                } catch (...) {
                }
            }
            // 安全收尾第 2 步 —— 平掉 poll 间隙/撤单竞态/跳变里被吃出的库存, 并校验+重试。
            // fire-and-forget 会在跳变薄盘口漏平 → 孤立仓位 (实测: 13:16 全市场跳变扫掉 7 池大单,
            // 发了平仓单却没等成交确认就退出 → 留 4 条腿)。改为最多 4 轮重试 (轮间等 1s 让盘口从跳变
            // 恢复), 平不掉的最后大声告警, 绝不静默带仓退出。
            if (use_live_path() && cfg_.live) {
                std::vector<std::pair<std::string, double>> residual;  // token -> 未平净额
                for (const auto& [tok, fl] : poll_fills()) {
                    double net = 0.0;
                    for (const auto& f : fl) net += (f.side == "BUY" ? 1.0 : -1.0) * f.size;
                    if (std::abs(net) >= 1e-9) residual.emplace_back(tok, net);
                }
                for (int round = 1; round <= 4 && !residual.empty(); ++round) {
                    std::vector<std::pair<std::string, double>> still;
                    for (const auto& [tok, net] : residual) {
                        const std::string side = net > 0 ? "SELL" : "BUY";
                        const json res = locked_submit(
                            {{"action", "FLATTEN"}, {"token_id", tok}, {"side", side}, {"size", std::abs(net)}});
                        const std::string st = res.value("status", std::string{});
                        std::fprintf(stderr, "shutdown: flatten net %.4f on %s (round %d): %s\n", net,
                                     tok.c_str(), round, st.c_str());
                        if (st != "FLATTENED") still.emplace_back(tok, net);  // 没平掉 → 下轮重试
                    }
                    residual.swap(still);
                    if (!residual.empty() && round < 4)
                        std::this_thread::sleep_for(std::chrono::seconds(1));  // 等盘口从跳变恢复
                }
                for (const auto& [tok, net] : residual)
                    std::fprintf(stderr,
                                 "shutdown: WARN ORPHAN net %.4f on %s — could NOT flatten; MANUAL FLATTEN NEEDED\n",
                                 net, tok.c_str());
            }
        } catch (...) {
        }
        engine_->close();
    }
    if (submitter_ != nullptr) {
        try {
            submitter_->close();
        } catch (...) {
        }
    }
    std::fprintf(stderr, "live-maker stopped.\n");
}

}  // namespace pmm
