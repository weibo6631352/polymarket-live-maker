// src/pmm/maker_live.cpp — live 做市策略 + dry-run 实现 (port of pm_trader/maker_live.py)
#include "pmm/maker_live.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string>

#include "pmm/models.hpp"
#include "pmm/orderbook.hpp"
#include "pmm/round.hpp"

namespace pmm::maker {

namespace ob = pmm::orderbook;

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
    return s;
}
}  // namespace

std::vector<Order> compute_two_sided_quotes(double mid, double half_spread_c, double size, double tick,
                                            double max_spread_c, double skew_ticks) {
    if (!(0.0 < mid && mid < 1.0)) {
        throw std::invalid_argument(std::format("mid must be in (0, 1), got {}", mid));
    }
    if (half_spread_c <= 0.0 || half_spread_c > max_spread_c) {
        throw std::invalid_argument(
            std::format("half_spread_c must be in (0, {}], got {}", max_spread_c, half_spread_c));
    }
    const double offset = half_spread_c / 100.0;
    const double shift = skew_ticks * tick;
    const double bid = std::max(tick, std::nearbyint((mid - offset - shift) / tick) * tick);
    const double ask = std::min(1.0 - tick, std::nearbyint((mid + offset - shift) / tick) * tick);
    return {Order{"BUY", round_to(bid, 4), size, std::string{}},
            Order{"SELL", round_to(ask, 4), size, std::string{}}};
}

std::vector<Order> compute_two_sided_quotes_yes_no(double mid, double half_spread_c, double size,
                                                   double tick, double max_spread_c,
                                                   const std::string& yes_token_id,
                                                   const std::string& no_token_id, double skew_ticks) {
    const std::vector<Order> yes =
        compute_two_sided_quotes(mid, half_spread_c, size, tick, max_spread_c, skew_ticks);
    std::vector<Order> out;
    out.reserve(yes.size());
    for (const auto& o : yes) {
        if (o.side == "SELL") {
            // SELL-YES @ ask  ≡  BUY-NO @ (1-ask)  (YES+NO=$1) — 全 USDC, 不需份额。
            // 夹到 [tick, 1-tick]: 防 ask 取整到 0 时 1-ask=1.0 越界。
            const double no_price =
                std::min(1.0 - tick, std::max(tick, round_to(1.0 - o.price, 4)));
            out.push_back(Order{"BUY", no_price, o.size, no_token_id});
        } else {
            out.push_back(Order{"BUY", o.price, o.size, yes_token_id});
        }
    }
    return out;
}

bool plan_requote(double mid_prev, double mid_now, double /*half_spread_c*/, double tick) {
    return std::abs(mid_now - mid_prev) >= tick;
}

std::int64_t gtd_expiration(double expiry_s, double now_unix) {
    return static_cast<std::int64_t>(now_unix) + std::max<std::int64_t>(static_cast<std::int64_t>(expiry_s), 60);
}

// ---------------------------------------------------------------------------
// LiveMakerBot
// ---------------------------------------------------------------------------

LiveMakerBot::LiveMakerBot(const MakerBotConfig& cfg, Submitter submitter)
    : token_id_(cfg.token_id),
      external_fills_(cfg.external_fills),
      max_spread_c_(cfg.max_spread_c),
      min_size_(cfg.min_size),
      tick_(cfg.tick),
      half_spread_c_(cfg.half_spread_c.value_or(cfg.tick * 100.0)),
      size_(cfg.size.value_or(cfg.min_size)),
      dry_run_(cfg.dry_run),
      jump_exit_ticks_(cfg.jump_exit_ticks.value_or(cfg.max_spread_c / (cfg.tick * 100.0))),
      max_inventory_(cfg.max_inventory.value_or(5.0 * cfg.size.value_or(cfg.min_size))),
      skew_strength_ticks_(cfg.skew_strength_ticks) {
    if (!dry_run_ && submitter == nullptr) {
        throw ApiError("Live mode requires an injected signer/submitter");
    }
    if (submitter != nullptr) {
        submitter_ = std::move(submitter);
    } else {
        submitter_ = [](const nlohmann::json& action) {
            nlohmann::json r = action;
            r["status"] = "DRY_RUN";
            return r;
        };
    }
}

double LiveMakerBot::skew_ticks() const {
    if (max_inventory_ <= 0.0) return 0.0;
    const double ratio = std::max(-1.0, std::min(1.0, inventory_ / max_inventory_));
    return skew_strength_ticks_ * ratio;
}

void LiveMakerBot::detect_fill(double mid) {
    if (last_ask_.has_value() && mid >= *last_ask_) {
        inventory_ -= size_;
    } else if (last_bid_.has_value() && mid <= *last_bid_) {
        inventory_ += size_;
    }
    if (max_inventory_ > 0.0) {
        inventory_ = std::max(-max_inventory_, std::min(max_inventory_, inventory_));
    }
}

void LiveMakerBot::apply_real_fill(const std::string& side, double size) {
    if (upper(side) == "BUY") {
        inventory_ += size;
    } else {
        inventory_ -= size;
    }
    if (max_inventory_ > 0.0) {
        inventory_ = std::max(-max_inventory_, std::min(max_inventory_, inventory_));
    }
}

MakerPlan LiveMakerBot::plan(const OrderBook& book, double mid) {
    const double skew = skew_ticks();
    std::vector<Order> quotes =
        compute_two_sided_quotes(mid, half_spread_c_, size_, tick_, max_spread_c_, skew);
    const bool requote =
        !last_mid_.has_value() || plan_requote(*last_mid_, mid, half_spread_c_, tick_);
    const double existing_qmin = ob::book_inband_qmin(book, mid, max_spread_c_);
    const double share = ob::maker_reward_share(size_, half_spread_c_, max_spread_c_, existing_qmin);

    MakerPlan p;
    p.token_id = token_id_;
    p.mid = mid;
    p.requote = requote;
    p.orders = std::move(quotes);
    p.est_reward_share = round_to(share, 4);
    p.committed_capital = round_to(ob::committed_capital(size_, half_spread_c_), 2);
    p.inventory = round_to(inventory_, 4);
    p.skew_ticks = round_to(skew, 4);
    p.dry_run = dry_run_;
    return p;
}

MakerPlan LiveMakerBot::halt_plan(double mid, std::vector<nlohmann::json> submitted) const {
    MakerPlan p;
    p.token_id = token_id_;
    p.mid = mid;
    p.requote = false;
    p.halted = true;
    p.recommend = "exit_cooldown";
    p.inventory = round_to(inventory_, 4);
    p.submitted = std::move(submitted);
    return p;
}

MakerPlan LiveMakerBot::step(const OrderBook& book, double mid) {
    if (halted_) {
        last_mid_ = mid;
        return halt_plan(mid, {});
    }
    const double moved = last_mid_.has_value() ? std::abs(mid - *last_mid_) : 0.0;
    if (!external_fills_) detect_fill(mid);

    if (moved >= jump_exit_ticks_ * tick_) {
        halted_ = true;
        std::vector<nlohmann::json> submitted;
        submitted.push_back(submitter_({{"action", "CANCEL_ALL"}, {"token_id", token_id_}}));
        last_mid_ = mid;
        return halt_plan(mid, std::move(submitted));
    }

    MakerPlan plan_ = plan(book, mid);
    plan_.halted = false;
    std::vector<nlohmann::json> submitted;
    if (plan_.requote) {
        if (last_mid_.has_value()) {
            submitted.push_back(submitter_({{"action", "CANCEL_ALL"}, {"token_id", token_id_}}));
        }
        for (const auto& o : plan_.orders) {
            submitted.push_back(submitter_({{"action", "PLACE"},
                                            {"token_id", token_id_},
                                            {"side", o.side},
                                            {"price", o.price},
                                            {"size", o.size}}));
        }
    }
    last_mid_ = mid;
    last_bid_ = plan_.orders[0].price;
    last_ask_ = plan_.orders[1].price;
    plan_.submitted = std::move(submitted);
    return plan_;
}

// ---------------------------------------------------------------------------
// DryRunSubmitter
// ---------------------------------------------------------------------------

nlohmann::json DryRunSubmitter::operator()(const nlohmann::json& action) {
    sent_.push_back(action);
    if (sent_.size() > 5000) sent_.pop_front();
    if (verbose_) {
        std::printf("DRY %s\n", action.dump().c_str());
    }
    nlohmann::json r = action;
    r["status"] = "OK";
    r["dry_run"] = true;
    return r;
}

// ---------------------------------------------------------------------------
// ConnectionWarmer
// ---------------------------------------------------------------------------

void ConnectionWarmer::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (started_ || stop_) return;  // 已 stop 则不再启 (避免遗留无人 join 的线程)
    started_ = true;
    thread_ = std::thread([this] { run(); });
}

void ConnectionWarmer::run() {
    for (;;) {
        std::unique_lock<std::mutex> lk(mu_);
        if (cv_.wait_for(lk, std::chrono::duration<double>(interval_), [this] { return stop_; })) {
            break;  // 被 stop 唤醒
        }
        lk.unlock();
        try {
            ping_();
            ticks_.fetch_add(1);
        } catch (...) {
            // keep-warm 尽力而为
        }
    }
}

void ConnectionWarmer::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;       // 已 stop: 线程已被前一次 stop 取走并 join
        stop_ = true;
        t = std::move(thread_);  // 锁内取走句柄, 避免与 start() 对 thread_ 的赋值竞态
    }
    cv_.notify_all();
    if (t.joinable()) t.join();
}

}  // namespace pmm::maker
