// apps/tail_seller_main.cpp — up-tail premium seller: rest GTC BUYs on NO tokens (= sell YES
// lottery tickets to takers), hold to resolution, redeem outside. NO re-quote, NO chase, NO speed.
//
// Strategy basis (2026-07-03 calibration, 32,570 resolved markets / 18 months): PM overprices
// crypto UP-tails (YES 2-7c) 4-5.5x at T-5..T-3 across ALL regimes; sell-up-wing portfolio was
// positive every quarter. Down-tails are fairly priced — do NOT sell them.
//
// SAFETY MODEL (per docs/EXECUTION-MECHANICS.md trial protocol + the scalp-runaway lesson:
// hard caps live on ORDER COUNT and DEPLOYED CAPITAL, never on lagging P&L):
//   - dry by default; LIVE only when PM_TRADER_LIVE=1 AND TS_ARM=1
//   - compile-time caps: MAX_ORDERS, MAX_NOTIONAL_USD; env TS_MAX_* can only LOWER them
//   - price sanity: BUY NO only at [0.80, 0.995] (an up-tail YES at 0.5-20c)
//   - full-chain JSONL log: validate -> balance -> place(ack,latency) -> fills -> hourly status
//   - STOP file kill-switch: `touch STOP_TAIL_SELLER` in cwd -> cancel all + exit
//   - on restart: reconciles via list_open_orders (no re-place of already-resting orders)
//
// Usage:
//   ./tail-seller orders.json                # dry: validate + print plan
//   PM_TRADER_LIVE=1 TS_ARM=1 ./tail-seller orders.json          # place + monitor
//   PM_TRADER_LIVE=1 TS_ARM=1 ./tail-seller orders.json cancel   # cancel all listed tokens' orders
// orders.json: [{"token_id":"123...","price":0.95,"size":10,"note":"btc-above-70k-jul-9"}, ...]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"

using nlohmann::json;

namespace {
constexpr int kMaxOrders = 8;             // hard ceiling — trial scale
constexpr double kMaxNotionalUsd = 60.0;  // hard ceiling on Σ price*size
constexpr double kMinPrice = 0.80;        // BUY NO band: up-tail YES = 0.5-20c
constexpr double kMaxPrice = 0.995;

std::atomic<bool> g_run{true};
void on_sig(int) { g_run.store(false); }

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void jlog(std::ofstream& f, json j) {
    j["t_ms"] = now_ms();
    f << j.dump() << "\n";
    f.flush();
    std::printf("%s\n", j.dump().c_str());
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: [PM_TRADER_LIVE=1 TS_ARM=1] tail-seller <orders.json> [cancel]\n");
        return 2;
    }
    const bool do_cancel = argc > 2 && std::string(argv[2]) == "cancel";

    // ---- load + validate the order sheet (dry and live both) ----
    json orders;
    {
        std::ifstream in(argv[1]);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 2;
        }
        try {
            in >> orders;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "bad json: %s\n", e.what());
            return 2;
        }
    }
    if (!orders.is_array() || orders.empty()) {
        std::fprintf(stderr, "orders.json must be a non-empty array\n");
        return 2;
    }
    double env_max_notional = kMaxNotionalUsd;
    int env_max_orders = kMaxOrders;
    if (const char* s = std::getenv("TS_MAX_NOTIONAL_USD")) env_max_notional = std::min(env_max_notional, std::atof(s));
    if (const char* s = std::getenv("TS_MAX_ORDERS")) env_max_orders = std::min(env_max_orders, std::atoi(s));

    double notional = 0.0;
    for (const auto& o : orders) {
        const std::string tok = o.value("token_id", "");
        const double px = o.value("price", 0.0);
        const double sz = o.value("size", 0.0);
        if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos) {
            std::fprintf(stderr, "VETO: bad token_id '%s'\n", tok.c_str());
            return 3;
        }
        if (px < kMinPrice || px > kMaxPrice) {
            std::fprintf(stderr, "VETO: price %.3f outside NO-buy band [%.2f, %.3f] (%s)\n", px,
                         kMinPrice, kMaxPrice, o.value("note", "").c_str());
            return 3;
        }
        if (sz < 1 || sz > 100) {
            std::fprintf(stderr, "VETO: size %.1f outside [1,100]\n", sz);
            return 3;
        }
        notional += px * sz;
    }
    if (static_cast<int>(orders.size()) > env_max_orders) {
        std::fprintf(stderr, "VETO: %zu orders > cap %d\n", orders.size(), env_max_orders);
        return 3;
    }
    if (notional > env_max_notional) {
        std::fprintf(stderr, "VETO: notional $%.2f > cap $%.2f\n", notional, env_max_notional);
        return 3;
    }
    std::printf("order sheet OK: %zu orders, $%.2f total collateral (caps: %d / $%.2f)\n",
                orders.size(), notional, env_max_orders, env_max_notional);

    const char* live_env = std::getenv("PM_TRADER_LIVE");
    const char* arm_env = std::getenv("TS_ARM");
    const bool armed = live_env && std::string(live_env) == "1" && arm_env && std::string(arm_env) == "1";
    if (!armed) {
        std::printf("DRY MODE (need PM_TRADER_LIVE=1 AND TS_ARM=1 to place). Plan:\n");
        for (const auto& o : orders)
            std::printf("  BUY %.1f @ %.3f  %s  (%s)\n", o.value("size", 0.0), o.value("price", 0.0),
                        o.value("token_id", "").substr(0, 18).c_str(), o.value("note", "").c_str());
        return 0;
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);
    pmm::clob::ClobSubmitter sub;
    if (!sub.ready()) {
        std::fprintf(stderr, "ClobSubmitter NOT ready (key/creds)\n");
        return 1;
    }
    std::ofstream lf("tail_seller_log.jsonl", std::ios::app);
    jlog(lf, {{"ev", "start"}, {"signer", sub.signer_address()}, {"orders", orders.size()},
              {"notional", notional}, {"cancel_mode", do_cancel}});

    // balance before anything
    if (auto bal = sub.usdc_balance()) {
        jlog(lf, {{"ev", "balance"}, {"usdc", *bal}});
        if (!do_cancel && *bal < notional) {
            jlog(lf, {{"ev", "abort"}, {"why", "balance below sheet notional"}});
            return 1;
        }
    }

    // reconcile: what already rests?
    auto open0 = sub.list_open_orders();
    jlog(lf, {{"ev", "open_orders_at_start"}, {"n", open0.size()}});

    if (do_cancel) {
        for (const auto& o : orders) {
            const json r = sub({{"action", "CANCEL_ALL"}, {"token_id", o.value("token_id", "")}});
            jlog(lf, {{"ev", "cancel"}, {"token", o.value("token_id", "")}, {"resp", r}});
        }
        return 0;
    }

    // place all (skip tokens that already have a resting order — idempotent restart)
    (void)sub.poll_fills();  // prime the fill cursor (execution-settlement-lag fix-set lesson)
    for (const auto& o : orders) {
        const std::string tok = o.value("token_id", "");
        bool resting = false;
        for (const auto& oo : open0)
            if (oo.value("asset_id", "") == tok) resting = true;
        if (resting) {
            jlog(lf, {{"ev", "skip_already_resting"}, {"token", tok}});
            continue;
        }
        sub.warm_token(tok);
        const auto t0 = std::chrono::steady_clock::now();
        const json r = sub({{"action", "PLACE"}, {"token_id", tok}, {"side", "BUY"},
                            {"price", o.value("price", 0.0)}, {"size", o.value("size", 0.0)}});
        const auto t1 = std::chrono::steady_clock::now();
        jlog(lf, {{"ev", "place"}, {"note", o.value("note", "")}, {"token", tok},
                  {"price", o.value("price", 0.0)}, {"size", o.value("size", 0.0)}, {"resp", r},
                  {"latency_ms", std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()}});
        if (r.value("status", "") != "PLACED") {
            jlog(lf, {{"ev", "place_rejected_stop"}, {"why", "one reject -> stop placing, keep log"}});
            break;  // conservative: investigate before continuing
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    // monitor loop: fills every 60s, status hourly, STOP file kill-switch
    int tick = 0;
    while (g_run.load()) {
        for (int i = 0; i < 60 && g_run.load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_run.load()) break;
        {
            std::ifstream stop("STOP_TAIL_SELLER");
            if (stop.good()) {
                jlog(lf, {{"ev", "stopfile"}, {"action", "cancel_all_and_exit"}});
                for (const auto& o : orders)
                    (void)sub({{"action", "CANCEL_ALL"}, {"token_id", o.value("token_id", "")}});
                break;
            }
        }
        for (const auto& f : sub.poll_fills()) jlog(lf, {{"ev", "fill"}, {"fill", f}});
        if (++tick % 60 == 0) {
            auto oo = sub.list_open_orders();
            json bal_j;
            if (auto bal = sub.usdc_balance()) bal_j = *bal;
            jlog(lf, {{"ev", "status"}, {"open_orders", oo.size()}, {"usdc", bal_j}});
        }
    }
    jlog(lf, {{"ev", "exit"}, {"note", "GTC orders (if any) remain resting by design; rerun to resume monitoring"}});
    return 0;
}
