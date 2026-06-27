// src/pmm/runner.cpp — 自主轮询循环实现 (port of pm_trader/runner.py LiveRunner)
#include "pmm/runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <thread>

#include "pmm/clob_submitter.hpp"
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
    : cfg_(std::move(cfg)),
      engine_(engine),
      scanner_(scanner),
      submitter_(submitter),
      sleeper_([](double s) {
          if (s > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(s));
      }) {
    if (cfg_.max_req_per_sec > 0.0) {
        rate_limiter_ = std::make_unique<RateLimiter>();  // per-endpoint 限额 (官方文档)
    }
    if (scanner_ == nullptr) {
        owned_scanner_ = std::make_unique<rewards::RewardsClient>(rate_limiter_.get());
        scanner_ = owned_scanner_.get();
    }
}

LiveRunner::~LiveRunner() {
    discovery_stop_.store(true);
    if (discovery_thread_.joinable()) discovery_thread_.join();
    if (resync_thread_.joinable()) resync_thread_.join();
}

rewards::RewardsClient& LiveRunner::scanner() { return *scanner_; }

void LiveRunner::event(const std::string& kind, const json& fields) {
    if (events_) events_->write(kind, fields);
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
    if (rate_limiter_) engine_->api().set_rate_limiter(rate_limiter_.get());

    if (cfg_.live) {
        if (submitter_ == nullptr) {
            owned_submitter_ = std::make_unique<clob::ClobSubmitter>(rate_limiter_.get());
            submitter_ = owned_submitter_.get();
        }
        std::fprintf(stderr, "LIVE submitter armed — real orders will be placed\n");
    } else if (cfg_.dry_live && submitter_ == nullptr) {
        owned_submitter_ = std::make_unique<maker::DryRunSubmitter>();
        submitter_ = owned_submitter_.get();
        std::fprintf(stderr, "DRY-LIVE: rehearsing the live code path (orders logged, not sent)\n");
    }

    start_ws();
    rehydrate();
    reconcile_broker_orders();
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
        double next_poll = mono_now();
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
                last_stats = now;  // (stats 日志略)
            }
            if (now - last_reeval >= cfg_.reeval_interval_s) {
                tick_cooldowns();
                reevaluate_held();
                reselect();
                last_reeval = now;
            }
            poll_once();
            next_poll += cfg_.poll_seconds;
            double delay = next_poll - mono_now();
            if (delay < 0.0) {
                next_poll = mono_now();  // 超预算 → 重锚, 不睡
                delay = 0.0;
            }
            // 可中断睡眠: 切片 (≤0.2s) 轮询 stop_/信号, SIGTERM 后 ~0.2s 内醒来走关停。
            // (旧实现 sleeper_(delay) 整睡, 信号要等满一个 poll 周期才被察觉。)
            while (delay > 0.0 && !stop_.load() && !g_signal_stop.load()) {
                const double slice = delay < 0.2 ? delay : 0.2;
                sleeper_(slice);
                delay -= slice;
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
    resync_thread_ = std::thread([this] {
        while (!discovery_stop_.load()) {
            std::vector<std::string> toks;
            {
                std::lock_guard<std::mutex> lk(reflex_mu_);
                for (const auto& [t, ref] : reflex_refs_) toks.push_back(t);
            }
            if (toks.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            for (const auto& t : toks) {  // 一轮一 token/book (rate-limiter 节流)
                if (discovery_stop_.load()) break;
                resync_once(t);
            }
        }
    });
}

void LiveRunner::resync_once(const std::string& token) {
    try {
        const json book = scanner().book(token);
        if (!book.empty() && market_ch_) {
            market_ch_->apply_rest_snapshot(token, book);
            compute_and_store_signals(token);  // 在新鲜权威盘口上算预测信号 (高频, 零额外请求)
        }
    } catch (...) {
    }
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
    const orderbook::BookSignals sig = orderbook::compute_book_signals(*ob, mid, v);
    if (!sig.valid) return;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        signal_by_token_[token] = sig;
    }
    // 标定日志: 每 token 节流 ~5s 记一次 (150Hz 全记会爆); mid/micro_price/obi + 下一周期 Δmid 供回归。
    const double now_m = mono_now();
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        auto it = signal_log_at_.find(token);
        if (it != signal_log_at_.end() && now_m - it->second < 5.0) return;
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

void LiveRunner::on_ws_price(const std::string& token, double mid) {
    std::string no_token;
    {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        auto it = reflex_refs_.find(token);
        if (it == reflex_refs_.end() || reflex_cancelled_.count(token) != 0) return;
        const auto [ref_mid, band] = it->second;
        if (std::abs(mid - ref_mid) < band) return;
        reflex_cancelled_.insert(token);  // 慢 I/O 前先占住
        auto cit = reflex_complement_.find(token);
        if (cit != reflex_complement_.end()) no_token = cit->second;
    }
    try {
        locked_submit({{"action", "CANCEL_ALL"}, {"token_id", token}});
        // YES mid 漂 → NO mid 反向漂同幅, BUY-NO 腿同样过时, 一并撤掉。
        if (!no_token.empty()) locked_submit({{"action", "CANCEL_ALL"}, {"token_id", no_token}});
        event("reflex_cancel", {{"token", token}, {"mid", mid}});
    } catch (...) {
        std::lock_guard<std::mutex> lk(reflex_mu_);
        reflex_cancelled_.erase(token);
    }
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
    try {
        rewards::ScanResult res = rewards::scan(scanner(), cfg_.min_daily, cfg_.scan_top, true,
                                                cfg_.min_days_to_resolution, cfg_.max_vol_mult);
        {
            std::lock_guard<std::mutex> lk(report_mu_);
            report_ = std::move(res);
            last_scan_ok_ = mono_now();
        }
        std::lock_guard<std::mutex> lk(report_mu_);
        std::fprintf(stderr, "discovery: %d safe of %d scored\n", report_.safe_count, report_.pools_scored);
    } catch (...) {
        // 保留上一份好 report; staleness guard 处理
    }
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
    p.cooldown = cd;
    {
        std::lock_guard<std::mutex> lk(report_mu_);
        selected_ = portfolio::select_pools(report_, p);
    }
    std::map<std::string, json> want;
    for (const auto& s : selected_) want[s.condition_id] = selected_to_json(s);

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
    int n_placed = 0, n_lowrew = 0, n_budget = 0, n_failed = 0;
    for (const auto& [cond, s] : want) {
        if (placed_.count(cond) != 0) continue;
        if (s.value("est_daily_reward", 0.0) < cfg_.min_pool_reward) {
            ++n_lowrew;
            continue;
        }
        const double cap = s.value("committed_capital", 0.0);
        if (committed + cap > cfg_.capital + 1e-6) {
            ++n_budget;
            continue;
        }
        try {
            place(cond, s);
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
    std::fprintf(stderr, "reselect: want=%d placed=%d (skip low_reward=%d over_budget=%d failed=%d)\n",
                 n_want, n_placed, n_lowrew, n_budget, n_failed);
    sync_ws_subscriptions();
}

void LiveRunner::place(const std::string& cond, const json& pool) {
    double hs = pool.value("half_spread_c", 0.0);
    if (cfg_.use_optimal_spread) {
        try {
            const json rec = engine().suggest_maker_half_spread(cond, "yes", 0.0, cfg_.poll_seconds);
            const double v = rec.value("half_spread_c", 0.0);
            if (v > 0.0) hs = v;
        } catch (...) {
        }
    }
    MakerQuoteOpts opts;
    opts.half_spread_cents = hs;
    const double sz = pool.value("size", 0.0);
    if (sz > 0.0) opts.size = sz;
    if (use_live_path()) {
        engine().place_maker_quote_live(cond, [this](const json& a) { return locked_submit(a); }, "yes", opts);
    } else {
        engine().place_maker_quote(cond, "yes", opts);
    }
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
        if (!fresh[i]) continue;
        const auto& [cond, q] = worklist[i];
        auto reason = degrade_reason(*fresh[i]);
        if (!reason && cfg_.max_mid_vel_cps > 0.0) {
            const double vel = mid_velocity(q.value("token_id", std::string{}));
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
    static const std::set<std::string> kCooldownReasons = {
        "jump_risk_rose", "empty_band", "reward_collapsed", "daily_cut", "one_sided", "fast_book"};
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
        out[f.value("token_id", std::string{})].push_back(rf);
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
        if (inv_pnl <= -cfg_.max_loss) {
            return "max-loss (maker inventory P&L $" + std::to_string(inv_pnl) + ")";
        }
    }
    if (cfg_.live && cfg_.min_wallet_usdc > 0.0 && submitter_ != nullptr) {
        const auto bal = submitter_->usdc_balance();
        if (bal && *bal < cfg_.min_wallet_usdc) {
            return "wallet-floor (real USDC $" + std::to_string(*bal) + ")";
        }
    }
    return std::nullopt;
}

void LiveRunner::shutdown() {
    discovery_stop_.store(true);
    if (discovery_thread_.joinable()) discovery_thread_.join();
    if (resync_thread_.joinable()) resync_thread_.join();
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
