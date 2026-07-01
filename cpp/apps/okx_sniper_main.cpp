// okx_sniper_main.cpp — EXTREME-low-latency crypto-micro LAG-SCALP.
// Edge (offline-verified: t=5.10, +6.8c/share, 79% win, n=57, stable over 51 windows): PM lags BTC ~200ms and the
// CHEAP favored side (ask<CHEAP_MAX) is where PM lagged MOST. BUY it on a BTC seconds-move, hold ~HOLD_MS for the
// catch-up, then SELL the bid. This is a SCALP, NOT a resolution bet (hold-to-resolution is -EV: the bounce reverts;
// PM resolves on CHAINLINK, Up wins iff end>=open). One position at a time; stop-loss caps the fat-tail reversal.
// Signal: OKX bbo-tbt WSS (leads ~200ms). Book: PM CLOB WSS best bid+ask per token (no REST poll in the hot path).
// Hard order-count + cumulative-$ caps + GIVEUP + tau>60 tail-guard (no stop-loss: settlement-lag makes it moot).
// SIGTERM/SIGINT -> graceful flatten. DRY unless double-gated LIVE. (the scalp-trial runaway lesson.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"
#include "pmm/net/ws_connection.hpp"

using json = nlohmann::json;

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run.store(false); }  // SIGTERM/SIGINT -> loop exits -> graceful flatten runs
static std::condition_variable g_cv;  // signalled by the feeds on a new tick -> wakes the hot loop (event-driven)
static std::mutex g_cv_mx;
static long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
static double env_d(const char* k, double d) {
    const char* v = std::getenv(k);
    return v ? std::atof(v) : d;
}
// PM taker fee per share per leg — verified on real fills; peaks at p=0.5. Charged on BOTH the buy and the sell,
// so a round-trip nets scalp - fee(entry) - fee(exit). Baking it into pnl keeps DRY honest (matches the fee-net
// replay) and makes the LIVE MAX_LOSS kill-switch a true NET cap rather than a loose gross one.
static double taker_fee(double p) { return 0.07 * p * (1.0 - p); }
static std::string http_get(const std::string& url) {
    const std::string cmd = "curl -s --max-time 4 '" + url + "'";
    std::string out; char buf[8192];
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}
static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- OKX signal feed (leads Binance ~200ms) ----
static std::mutex okx_mx;
static std::deque<std::pair<long, double>> g_okx;  // (t_ms, mid)
static double okx_now() {
    std::lock_guard<std::mutex> lk(okx_mx);
    return g_okx.empty() ? 0.0 : g_okx.back().second;
}
static double okx_ago(long ms) {
    const long tgt = now_ms() - ms;
    std::lock_guard<std::mutex> lk(okx_mx);
    double v = 0.0;
    for (const auto& p : g_okx) { if (p.first <= tgt) v = p.second; else break; }
    return v;
}
// 3s return, but 0.0 if the feed is STALE (>1.5s since last tick) — guards the latched-signal
// misfire (a feed that freezes mid-move would otherwise keep mv>THRESH forever). (review I2)
static double okx_move() {
    std::lock_guard<std::mutex> lk(okx_mx);
    if (g_okx.empty() || now_ms() - g_okx.back().first > 1500) return 0.0;
    const double n = g_okx.back().second;
    const long tgt = now_ms() - 3000;
    double a = 0.0;
    for (const auto& p : g_okx) { if (p.first <= tgt) a = p.second; else break; }
    return (n > 0 && a > 0) ? (n / a - 1.0) : 0.0;
}
static void okx_feed() {
    while (g_run.load()) {
        pmm::net::WsConnection ws("ws.okx.com", "/ws/v5/public", 3000);
        if (!ws.connect()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        (void)ws.send_text("{\"op\":\"subscribe\",\"args\":[{\"channel\":\"bbo-tbt\",\"instId\":\"BTC-USDT\"}]}");
        std::printf("[OKX] connected (bbo-tbt)\n");
        long lastping = now_ms();
        while (g_run.load()) {
            if (now_ms() - lastping > 20000) { (void)ws.send_text("ping"); lastping = now_ms(); }
            auto m = ws.recv();
            if (m.kind == pmm::net::WsMessage::Closed) break;
            if (m.kind != pmm::net::WsMessage::Text) continue;
            try {
                auto j = json::parse(m.text);
                if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
                    auto& d = j["data"][0];
                    if (d.contains("bids") && !d["bids"].empty() && d.contains("asks") && !d["asks"].empty()) {
                        const double mid = (std::stod(d["bids"][0][0].get<std::string>()) +
                                            std::stod(d["asks"][0][0].get<std::string>())) / 2.0;
                        { std::lock_guard<std::mutex> lk(okx_mx); g_okx.emplace_back(now_ms(), mid); if (g_okx.size() > 4000) g_okx.pop_front(); }
                        g_cv.notify_one();  // event-driven: wake the hot loop the instant a new tick arrives
                    }
                }
            } catch (...) {}
        }
        ws.close();
    }
}

// ---- Binance feed (alternative / confirmation signal — the resolution venue) ----
static std::mutex btc_mx;
static std::deque<std::pair<long, double>> g_btc;
static double btc_move() {  // freshness-gated 3s return (review I2)
    std::lock_guard<std::mutex> lk(btc_mx);
    if (g_btc.empty() || now_ms() - g_btc.back().first > 1500) return 0.0;
    const double n = g_btc.back().second;
    const long tgt = now_ms() - 3000;
    double a = 0.0;
    for (const auto& p : g_btc) { if (p.first <= tgt) a = p.second; else break; }
    return (n > 0 && a > 0) ? (n / a - 1.0) : 0.0;
}
static void binance_feed() {
    while (g_run.load()) {
        pmm::net::WsConnection ws("stream.binance.com", "/ws/btcusdt@bookTicker", 3000);
        if (!ws.connect()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        std::printf("[BIN] connected\n");
        while (g_run.load()) {
            auto m = ws.recv();
            if (m.kind == pmm::net::WsMessage::Closed) break;
            if (m.kind != pmm::net::WsMessage::Text) continue;
            try {
                auto j = json::parse(m.text);
                if (j.contains("b") && j.contains("a")) {
                    const double mid = (std::stod(j["b"].get<std::string>()) +
                                        std::stod(j["a"].get<std::string>())) / 2.0;
                    { std::lock_guard<std::mutex> lk(btc_mx); g_btc.emplace_back(now_ms(), mid); if (g_btc.size() > 4000) g_btc.pop_front(); }
                    g_cv.notify_one();
                }
            } catch (...) {}
        }
        ws.close();
    }
}

// ---- PM CLOB book (real-time best_ask for the current window's tokens) ----
static std::mutex book_mx;
static std::string g_up_tok, g_dn_tok;
static double g_up_ask = 0.0, g_dn_ask = 0.0;
static double g_up_bid = 0.0, g_dn_bid = 0.0;  // bid too — the scalp EXITS by selling the bid
static long g_up_t = 0, g_dn_t = 0;  // last book update PER SIDE — the freshness gate must use the side we hold/enter,
                                     // not a shared clock (a fresh up-tick must NOT mask a stale dn-bid we're selling)
static std::atomic<long> g_epoch{0};  // bump on window change -> pm_book resubscribes

static void pm_book() {
    long my_epoch = -1;
    while (g_run.load()) {
        std::string up, dn;
        long ep = g_epoch.load();
        { std::lock_guard<std::mutex> lk(book_mx); up = g_up_tok; dn = g_dn_tok; }
        if (up.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        pmm::net::WsConnection ws("ws-subscriptions-clob.polymarket.com", "/ws/market", 2000);
        if (!ws.connect()) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        (void)ws.send_text("{\"assets_ids\":[\"" + up + "\",\"" + dn + "\"],\"type\":\"market\"}");
        my_epoch = ep;
        std::printf("[PMBOOK] subscribed up=%s..\n", up.substr(0, 10).c_str());
        long lastping = now_ms();
        while (g_run.load() && g_epoch.load() == my_epoch) {
            if (now_ms() - lastping > 9000) { (void)ws.send_text("PING"); lastping = now_ms(); }
            auto m = ws.recv();
            if (m.kind == pmm::net::WsMessage::Closed) break;
            if (m.kind != pmm::net::WsMessage::Text) continue;
            try {
                auto j = json::parse(m.text);
                auto ev = (j.is_array() && !j.empty()) ? j[0] : j;
                const std::string et = ev.value("event_type", std::string());
                auto setq = [&](const std::string& aid, double bid, double ask) {  // -1 = leave unchanged
                    std::lock_guard<std::mutex> lk(book_mx);
                    if (aid == g_up_tok) { if (ask > 0) g_up_ask = ask; if (bid > 0) g_up_bid = bid; g_up_t = now_ms(); }
                    else if (aid == g_dn_tok) { if (ask > 0) g_dn_ask = ask; if (bid > 0) g_dn_bid = bid; g_dn_t = now_ms(); }
                };
                if (et == "book") {
                    const std::string aid = ev.value("asset_id", std::string());
                    double bestask = 1e9, bestbid = 0.0;
                    for (auto& a : ev["asks"]) bestask = std::min(bestask, std::stod(a["price"].get<std::string>()));
                    for (auto& b : ev["bids"]) bestbid = std::max(bestbid, std::stod(b["price"].get<std::string>()));
                    setq(aid, bestbid, bestask < 1e9 ? bestask : -1.0);
                } else if (et == "price_change") {
                    for (auto& pc : ev["price_changes"]) {
                        const std::string aid = pc.value("asset_id", std::string());
                        const std::string as = pc.value("best_ask", std::string());
                        const std::string bs = pc.value("best_bid", std::string());
                        setq(aid, bs.empty() ? -1.0 : std::stod(bs), as.empty() ? -1.0 : std::stod(as));
                    }
                }
            } catch (...) {}
        }
        ws.close();
    }
}

struct Window { std::string up_tok, dn_tok; double end_unix{0}; bool valid{false}; };

// One open scalp position at a time. We BUY the cheap favored-side ask, then SELL the bid after the hold /
// on stop-loss (the lag catch-up). NEVER held to resolution (that's the falsified -EV path).
struct Position { bool open{false}; bool up{false}; std::string tok; double entry_ask{0}; double shares{0}; long entry_t{0}; long sell_t{0}; };

static Window discover() {
    Window w;
    std::time_t now = std::time(nullptr);
    char iso[32];
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    const std::string body = http_get(
        std::string("https://gamma-api.polymarket.com/markets?closed=false&limit=200&order=endDate&ascending=true&end_date_min=") + iso);
    try {
        auto arr = json::parse(body);
        double best_tau = -1;
        for (auto& m : arr) {
            const std::string q = lower(m.value("question", std::string()));
            if (q.find("up or down") == std::string::npos || q.find("bitcoin") == std::string::npos) continue;
            if (!m.value("acceptingOrders", false)) continue;
            const std::string tk = m.value("clobTokenIds", std::string());
            if (tk.empty()) continue;
            std::tm tmv{};
            if (sscanf(m["endDate"].get<std::string>().c_str(), "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon,
                       &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6) continue;
            tmv.tm_year -= 1900; tmv.tm_mon -= 1;
            const double end_unix = static_cast<double>(timegm(&tmv));
            const double tau = end_unix - static_cast<double>(now);
            if (tau < 120 || tau > 300) continue;
            if (tau > best_tau) {
                auto toks = json::parse(tk);
                w.up_tok = toks[0]; w.dn_tok = toks[1]; w.end_unix = end_unix;
                best_tau = tau; w.valid = true;
            }
        }
    } catch (...) {}
    return w;
}

// ---- background discovery: keep the hot thread off the blocking popen/curl (review R1) ----
static std::mutex next_mx;
static Window g_next;
static void discover_thread() {
    while (g_run.load()) {
        Window nw = discover();
        if (nw.valid) { std::lock_guard<std::mutex> lk(next_mx); g_next = nw; }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGINT, on_signal);   // graceful shutdown: set g_run=false -> loop exits -> flatten runs (no orphan)
    std::signal(SIGTERM, on_signal);
    const double THRESH = env_d("SNIPE_THRESH", 0.0003);
    const int MAX_TRADES = static_cast<int>(env_d("SNIPE_MAX_TRADES", 60));  // ATTEMPT backstop (incl FAK-killed)
    const int MAX_FILLS = static_cast<int>(env_d("SNIPE_MAX_FILLS", 20));    // ACTUAL-fill target — the real "N 单" goal
    const double MAX_USD = env_d("SNIPE_MAX_USD", 10.0);  // cumulative-deployed risk cap
    double SHARES = env_d("SNIPE_SHARES", 5.0);  // PM market minimum = 5 shares; never below
    if (SHARES < 5.0) SHARES = 5.0;
    const double CHEAP_MAX = env_d("SNIPE_CHEAP_MAX", 0.55);  // only enter the CHEAP favored side (max lag = max edge)
    const long HOLD_MS = static_cast<long>(env_d("SNIPE_HOLD_MS", 1500));  // scalp hold before selling the bid
    const long MAX_HOLD_MS = static_cast<long>(env_d("SNIPE_MAX_HOLD_MS", 8000));  // HARD force-exit even on a stale book
    const long GIVEUP_MS = static_cast<long>(env_d("SNIPE_GIVEUP_MS", 20000));  // catch-all: abandon a sell stuck > this (any reason)
    const long COOLDOWN_MS = static_cast<long>(env_d("SNIPE_COOLDOWN_MS", 2000));  // min gap after an exit before re-entry
    const double MAX_LOSS = env_d("SNIPE_MAX_LOSS", 3.0);  // realized-loss kill-switch: stop entering once pnl <= -MAX_LOSS
    const bool LIVE = (std::getenv("PM_TRADER_LIVE") && std::string(std::getenv("PM_TRADER_LIVE")) == "1") &&
                      (std::getenv("LM_SNIPE_ARM") && std::string(std::getenv("LM_SNIPE_ARM")) == "1");
    pmm::app::LoadDotEnv(".env", LIVE);
    std::printf("=== okx-sniper LAG-SCALP ===\nmode=%s thresh=%.4f cheap<%.2f hold=%ldms tau>60 fills=%d attempts=%d shares=%.0f\n",
                LIVE ? "LIVE-REAL-MONEY" : "DRY", THRESH, CHEAP_MAX, HOLD_MS, MAX_FILLS, MAX_TRADES, SHARES);

    pmm::clob::ClobSubmitter sub;
    if (LIVE && !sub.ready()) { std::printf("LIVE but ClobSubmitter not ready — abort\n"); return 1; }

    const std::string SRC = std::getenv("SNIPE_SRC") ? std::getenv("SNIPE_SRC") : "okx";  // okx | bin | both
    std::printf("signal=%s\n", SRC.c_str());
    std::thread to(okx_feed), tb(pm_book), td(discover_thread);
    std::thread tbin;
    if (SRC != "okx") tbin = std::thread(binance_feed);

    int trades = 0;         // buy ATTEMPTS (counted before place — hard backstop; FAK-killed ones count here)
    int fills = 0;          // ACTUAL fills (FAK-killed don't count) — the real "满 N 单" target
    double deployed = 0.0;  // cumulative REAL $ deployed (only on an actual fill) — the hard risk cap
    double pnl = 0.0;       // cumulative realized scalp P&L (DRY: simulated from bid-ask)
    bool armed = true;      // edge-trigger: fire once per move, re-arm when it subsides
    long last_hb = 0;
    long last_exit = 0;     // last scalp-exit time — enforces a re-entry cooldown (anti-churn)
    Window w;
    Position pos;           // one open scalp at a time (enter flat, exit by selling the bid)
    while (g_run.load() && (pos.open || (fills < MAX_FILLS && trades < MAX_TRADES && deployed < MAX_USD))) {
        // NEVER swap the window while holding — that would strand the position on the wrong token's book
        if (!pos.open && (!w.valid || static_cast<double>(std::time(nullptr)) > w.end_unix - 25)) {
            Window cand;
            { std::lock_guard<std::mutex> lk(next_mx); cand = g_next; }  // non-blocking — discovery runs off-thread (R1)
            if (cand.valid && cand.up_tok != w.up_tok) {
                w = cand;
                { std::lock_guard<std::mutex> lk(book_mx); g_up_tok = w.up_tok; g_dn_tok = w.dn_tok; g_up_ask = 0; g_dn_ask = 0; g_up_bid = 0; g_dn_bid = 0; }
                g_epoch.fetch_add(1);
                armed = true;  // fresh window = fresh opportunity
                std::printf("[WINDOW] up=%s.. end_in=%.0fs\n", w.up_tok.substr(0, 12).c_str(),
                            w.end_unix - static_cast<double>(std::time(nullptr)));
            } else if (!w.valid) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            // else: current window near-end but no fresh one yet — keep w (the tau>20 gate blocks late trades)
        }
        // hot path: ENTRY when flat (cheap-favored-side), EXIT when holding (sell the bid = the scalp)
        const long t = now_ms();
        if (!pos.open) {
            double mv = okx_move();
            if (SRC == "bin") mv = btc_move();
            else if (SRC == "both" && std::fabs(mv) <= THRESH) mv = btc_move();
            if (std::fabs(mv) < THRESH * 0.5) armed = true;  // re-arm once the move subsides (hysteresis)
            if (std::fabs(mv) > THRESH && armed && deployed < MAX_USD && t - last_exit > COOLDOWN_MS && pnl > -MAX_LOSS) {
                const bool up = mv > 0;
                const std::string fav = up ? w.up_tok : w.dn_tok;
                double ask; long ask_t;
                { std::lock_guard<std::mutex> lk(book_mx); ask = up ? g_up_ask : g_dn_ask; ask_t = up ? g_up_t : g_dn_t; }
                const double tau = w.end_unix - static_cast<double>(std::time(nullptr));
                const double notional = SHARES * ask;
                // gates: CHEAP favored side (max lag = the edge), TAU>60s so the forced ~3.5s settlement-hold can
                // NEVER reach window-end -> kills the resolution-to-0 tail (cheap = the losing side), >= $1 notional, ask fresh
                if (ask > 0.03 && ask < CHEAP_MAX && tau > 60 && notional >= 1.05 && t - ask_t < 2000) {
                    armed = false;                                    // edge-trigger: one fire per move-event
                    const double buy_px = std::min(ask + 0.03, 0.97); // marketable: cross the ask so it TAKES (C1)
                    ++trades;                                         // count the ATTEMPT before placing (S1 backstop)
                    bool ok_buy = true; std::string st = "DRY"; double got = SHARES;
                    if (LIVE) {
                        // FAK (fill-and-kill): takes what's immediately available, cancels the rest — it can NEVER
                        // rest as a live order, so a buy that returns filled=0 truly bought nothing (no async orphan).
                        // A GTC marketable buy that didn't cross rests, later fills async, and orphans (live-caught bug).
                        static const std::string OT = std::getenv("SNIPE_ORDER_TYPE") ? std::getenv("SNIPE_ORDER_TYPE") : "FAK";
                        const auto r = sub({{"action", "PLACE"}, {"token_id", fav}, {"side", "BUY"},
                                            {"price", buy_px}, {"size", SHARES}, {"order_type", OT}});
                        st = r.value("status", std::string());
                        got = r.value("filled", 0.0);   // with FAK this is the DEFINITIVE fill (0..SHARES), not a snapshot
                        std::printf("[BUY-RESP] ot=%s filled=%.4f status=%s http=%d resp=%.220s\n", OT.c_str(), got,
                                    st.c_str(), r.value("http", 0), r.value("resp", std::string()).c_str());
                        ok_buy = (st != "REJECTED" && st != "ERROR") && got >= 1.0;
                    }
                    // sell EXACTLY what filled (floor 0.01 to dodge balance-rounding rejects) — selling the intended
                    // SHARES when only got<SHARES filled = the infinite "balance not enough" retry that stranded a pos to 0.
                    const double held = LIVE ? std::floor(got * 100.0) / 100.0 : SHARES;
                    if (ok_buy) {  // a REAL fill (FAK-killed buys skip this) — only now count the fill + real $ deployed
                        pos = {true, up, fav, LIVE ? buy_px : ask, held, t, t + HOLD_MS};
                        ++fills; deployed += held * (LIVE ? buy_px : ask);
                    }
                    const std::string tail = LIVE ? (" status=" + st) : std::string("  (hold then sell bid)");
                    std::printf("[BUY-%s] mv=%+.3f%% %s ask=%.3f buy=%.3f $%.2f tau=%.0fs%s\n",
                                LIVE ? "LIVE" : "DRY", mv * 100, up ? "Up" : "Down", ask, buy_px, notional, tau, tail.c_str());
                }
            }
        } else {
            // SCALP EXIT: sell on stop / hold / window-end. A HARD force-exit (max-hold OR window-end) fires even if
            // the book is stale/empty — a frozen book must NEVER strand us into the falsified resolution bet (CRIT-1).
            double bid; long bid_t;
            { std::lock_guard<std::mutex> lk(book_mx); bid = pos.up ? g_up_bid : g_dn_bid; bid_t = pos.up ? g_up_t : g_dn_t; }
            const double tau = w.end_unix - static_cast<double>(std::time(nullptr));
            const bool fresh = (bid > 0 && t - bid_t < 3000);
            const bool timeup = (t >= pos.sell_t);
            const bool force = (t - pos.entry_t >= MAX_HOLD_MS) || tau < 12;  // HARD — ignores book freshness
            // NO stop-loss: under the ~3.5s settlement lag it can't fire before the timeup exit (both exit at the
            // settled bid); live it only relabelled trades the no-stop replay keeps. tau>60 handles the tail instead.
            if ((timeup && fresh) || force) {
                const double exit_px = fresh ? bid : std::max(pos.entry_ask - 0.05, 0.01);  // floor if no fresh bid
                std::string st = "DRY", resp; int http = 0;
                bool sold = true;
                if (LIVE) {
                    const double sell_px = std::max(exit_px - 0.03, 0.01);  // cross 3 ticks below bid -> DEFINITELY takes (rule out a price miss)
                    const auto r = sub({{"action", "PLACE"}, {"token_id", pos.tok}, {"side", "SELL"},
                                        {"price", sell_px}, {"size", pos.shares}});
                    st = r.value("status", std::string());
                    resp = r.value("resp", std::string()); http = r.value("http", 0);  // raw CLOB error -> price vs no-shares
                    sold = (st != "REJECTED" && st != "ERROR");  // CRIT-2: on reject KEEP the position + retry next loop
                }
                if (sold) {
                    const double scalp = exit_px - pos.entry_ask;
                    const double net_sh = scalp - taker_fee(pos.entry_ask) - taker_fee(exit_px);  // both legs' taker fee -> true net
                    pnl += net_sh * pos.shares;
                    const std::string tail = LIVE ? (" status=" + st) : std::string();
                    std::printf("[SELL-%s] %s entry=%.3f exit=%.3f net=%+.4f/sh ($%+.3f fee-in) held=%ldms reason=%s cumPnL=$%+.3f%s\n",
                                LIVE ? "LIVE" : "DRY", pos.up ? "Up" : "Down", pos.entry_ask, exit_px, net_sh, net_sh * pos.shares,
                                t - pos.entry_t, force ? (tau < 12 ? "win-end" : "max-hold") : "hold", pnl, tail.c_str());
                    pos = Position{}; last_exit = t; armed = false;  // cooldown + require the move to subside (anti-churn)
                } else {
                    std::printf("[SELL-RETRY-LIVE] %s status=%s http=%d resp=%s\n",
                                pos.up ? "Up" : "Down", st.c_str(), http, resp.substr(0, 140).c_str());
                    // GIVE UP the retry if: window resolved (token dead) OR stuck > GIVEUP_MS (catch-all, ANY reason).
                    // Guarantees the bot can never freeze on a position > GIVEUP_MS regardless of the failure cause.
                    if (resp.find("invalid token") != std::string::npos || t - pos.entry_t > GIVEUP_MS) {
                        pnl -= (pos.entry_ask + taker_fee(pos.entry_ask)) * pos.shares;  // assume lost: cost + entry fee already paid (matches replay's resolved-loss)
                        std::printf("[GIVEUP-LIVE] %s abandoned after %ldms (resp=%.40s) assume loss $%.3f cumPnL=$%+.3f\n",
                                    pos.up ? "Up" : "Down", t - pos.entry_t, resp.c_str(), pos.entry_ask * pos.shares, pnl);
                        pos = Position{}; last_exit = t; armed = false;
                    }
                }
            }
        }
        if (t - last_hb > 12000) {
            const double pn = okx_now(), pa = okx_ago(3000);
            double ua, da;
            { std::lock_guard<std::mutex> lk(book_mx); ua = g_up_ask; da = g_dn_ask; }
            std::printf("[HB] okx_now=%.1f mv3s=%+.4f%% up_ask=%.3f dn_ask=%.3f pos=%s pnl=$%+.3f\n",
                        pn, (pa > 0 ? (pn / pa - 1) * 100 : 0.0), ua, da,
                        pos.open ? (pos.up ? "Up" : "Down") : "flat", pnl);
            last_hb = t;
        }
        {  // EVENT-DRIVEN: wake the instant a feed pushes a new tick; 50ms fallback for window/tau/HB checks
            std::unique_lock<std::mutex> lk(g_cv_mx);
            g_cv.wait_for(lk, std::chrono::milliseconds(50));
        }
    }
    if (pos.open) {  // graceful-shutdown flatten — never orphan a LIVE position; CHECK the sell, retry, warn if it fails
        bool flat = !LIVE;
        for (int i = 0; i < 5 && !flat; ++i) {
            double bid; { std::lock_guard<std::mutex> lk(book_mx); bid = pos.up ? g_up_bid : g_dn_bid; }
            const double exit_px = (bid > 0) ? bid : std::max(pos.entry_ask - 0.05, 0.01);
            const auto r = sub({{"action", "PLACE"}, {"token_id", pos.tok}, {"side", "SELL"},
                                {"price", std::max(exit_px - 0.01, 0.01)}, {"size", pos.shares}});
            const std::string st = r.value("status", std::string());
            if (st != "REJECTED" && st != "ERROR") {
                flat = true; pnl += ((exit_px - pos.entry_ask) - taker_fee(pos.entry_ask) - taker_fee(exit_px)) * pos.shares;
                std::printf("[FLATTEN-LIVE] %s exit=%.3f\n", pos.up ? "Up" : "Down", exit_px);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        }
        if (!LIVE) std::printf("[FLATTEN-DRY] %s (sim)\n", pos.up ? "Up" : "Down");
        if (!flat) std::printf("[ORPHAN-WARNING] %s tok=%s SELL FAILED on shutdown — position MAY BE OPEN ON-CHAIN, reconcile manually!\n",
                               pos.up ? "Up" : "Down", pos.tok.substr(0, 14).c_str());
        pos = Position{};
    }
    std::printf("DONE: %d fills / %d attempts (targets %d/%d) cumPnL=$%+.3f\n", fills, trades, MAX_FILLS, MAX_TRADES, pnl);
    g_run.store(false);
    to.join();
    tb.join();
    td.join();
    if (tbin.joinable()) tbin.join();
    return 0;
}
