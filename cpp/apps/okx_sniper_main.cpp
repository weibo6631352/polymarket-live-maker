// okx_sniper_main.cpp — EXTREME-low-latency crypto-micro sniper.
// Signal: OKX BTC-USDT tickers WSS (leads Binance ~200ms; the latency-sensitive edge that REWARDS speed).
// Book:   PM CLOB WSS, real-time best_ask per token (369/s, no REST poll in the hot path).
// Hot path: OKX seconds-move (>THRESH/3s) -> BUY the favored side at the live PM ask -> PM resolves on Binance.
// Busy-poll 1ms (not 200ms). Hard order-count cap (scalp-trial runaway lesson). DRY unless double-gated LIVE.
// Latency budget: OKX detect (~ms) + decision (us) + signed order over a hot connection. No fair model — raw dir.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
static long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
static double env_d(const char* k, double d) {
    const char* v = std::getenv(k);
    return v ? std::atof(v) : d;
}
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
        (void)ws.send_text("{\"op\":\"subscribe\",\"args\":[{\"channel\":\"tickers\",\"instId\":\"BTC-USDT\"}]}");
        std::printf("[OKX] connected\n");
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
                    const double mid = (std::stod(d["bidPx"].get<std::string>()) +
                                        std::stod(d["askPx"].get<std::string>())) / 2.0;
                    std::lock_guard<std::mutex> lk(okx_mx);
                    g_okx.emplace_back(now_ms(), mid);
                    if (g_okx.size() > 4000) g_okx.pop_front();
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
                    std::lock_guard<std::mutex> lk(btc_mx);
                    g_btc.emplace_back(now_ms(), mid);
                    if (g_btc.size() > 4000) g_btc.pop_front();
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
static long g_ask_t = 0;  // last time a current-window ask was updated (review I2 ask freshness)
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
                auto setask = [&](const std::string& aid, double a) {
                    std::lock_guard<std::mutex> lk(book_mx);
                    if (aid == g_up_tok) { g_up_ask = a; g_ask_t = now_ms(); }
                    else if (aid == g_dn_tok) { g_dn_ask = a; g_ask_t = now_ms(); }
                };
                if (et == "book") {
                    const std::string aid = ev.value("asset_id", std::string());
                    double best = 1e9;
                    for (auto& a : ev["asks"]) best = std::min(best, std::stod(a["price"].get<std::string>()));
                    if (best < 1e9) setask(aid, best);
                } else if (et == "price_change") {
                    for (auto& pc : ev["price_changes"]) {
                        const std::string aid = pc.value("asset_id", std::string());
                        const std::string bs = pc.value("best_ask", std::string());
                        if (!bs.empty()) { const double ba = std::stod(bs); if (ba > 0) setask(aid, ba); }
                    }
                }
            } catch (...) {}
        }
        ws.close();
    }
}

struct Window { std::string up_tok, dn_tok; double end_unix{0}; bool valid{false}; };

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

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const double THRESH = env_d("SNIPE_THRESH", 0.0003);
    const int MAX_TRADES = static_cast<int>(env_d("SNIPE_MAX_TRADES", 30));  // count backstop
    const double MAX_USD = env_d("SNIPE_MAX_USD", 10.0);  // POSITION limit — the real risk bound (deployed $)
    double SHARES = env_d("SNIPE_SHARES", 5.0);  // PM market minimum = 5 shares; never below
    if (SHARES < 5.0) SHARES = 5.0;
    const bool LIVE = (std::getenv("PM_TRADER_LIVE") && std::string(std::getenv("PM_TRADER_LIVE")) == "1") &&
                      (std::getenv("LM_SNIPE_ARM") && std::string(std::getenv("LM_SNIPE_ARM")) == "1");
    pmm::app::LoadDotEnv(".env", LIVE);
    std::printf("=== okx-sniper (extreme-low-latency) ===\nmode=%s thresh=%.4f max=%d shares=%.0f(min5)\n",
                LIVE ? "LIVE-REAL-MONEY" : "DRY", THRESH, MAX_TRADES, SHARES);

    pmm::clob::ClobSubmitter sub;
    if (LIVE && !sub.ready()) { std::printf("LIVE but ClobSubmitter not ready — abort\n"); return 1; }

    const std::string SRC = std::getenv("SNIPE_SRC") ? std::getenv("SNIPE_SRC") : "okx";  // okx | bin | both
    std::printf("signal=%s\n", SRC.c_str());
    std::thread to(okx_feed), tb(pm_book);
    std::thread tbin;
    if (SRC != "okx") tbin = std::thread(binance_feed);

    int trades = 0;
    double deployed = 0.0;  // total $ deployed — the POSITION limit (replaces the crude 2s cooldown)
    bool armed = true;      // edge-trigger: fire once per move, re-arm when it subsides
    long last_hb = 0;
    Window w;
    while (g_run.load() && trades < MAX_TRADES && deployed < MAX_USD) {
        if (!w.valid || static_cast<double>(std::time(nullptr)) > w.end_unix - 25) {
            Window nw = discover();
            if (nw.valid) {
                w = nw;
                { std::lock_guard<std::mutex> lk(book_mx); g_up_tok = w.up_tok; g_dn_tok = w.dn_tok; g_up_ask = 0; g_dn_ask = 0; }
                g_epoch.fetch_add(1);
                armed = true;  // fresh window = fresh opportunity
                std::printf("[WINDOW] up=%s.. end_in=%.0fs\n", w.up_tok.substr(0, 12).c_str(),
                            w.end_unix - static_cast<double>(std::time(nullptr)));
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
        }
        // hot signal: freshness-gated seconds-move, EDGE-TRIGGERED (fire once per move, no 2s cooldown)
        const long t = now_ms();
        {
            double mv = okx_move();
            if (SRC == "bin") mv = btc_move();
            else if (SRC == "both" && std::fabs(mv) <= THRESH) mv = btc_move();
            if (std::fabs(mv) < THRESH * 0.5) armed = true;  // re-arm once the move subsides (hysteresis)
            if (std::fabs(mv) > THRESH && armed) {
                const bool up = mv > 0;
                const std::string fav = up ? w.up_tok : w.dn_tok;
                double ask; long ask_t;
                { std::lock_guard<std::mutex> lk(book_mx); ask = up ? g_up_ask : g_dn_ask; ask_t = g_ask_t; }
                const double tau = w.end_unix - static_cast<double>(std::time(nullptr));
                const double notional = SHARES * ask;
                // gates: sane ask, time left, >= $1 notional (I1), ask fresh < 2s (I2)
                if (ask > 0.03 && ask < 0.97 && tau > 20 && notional >= 1.05 && t - ask_t < 2000) {
                    armed = false;                                    // edge-trigger: one fire per move-event
                    const double buy_px = std::min(ask + 0.03, 0.97); // marketable: cross the ask so it TAKES (C1)
                    ++trades;                                         // count intent-to-place BEFORE placing (S1)
                    deployed += notional;                             // position-limit accounting
                    if (LIVE) {
                        const auto r = sub({{"action", "PLACE"}, {"token_id", fav}, {"side", "BUY"},
                                            {"price", buy_px}, {"size", SHARES}});
                        const std::string st = r.value("status", std::string());
                        std::printf("[SNIPE-LIVE] mv=%+.3f%% fav=%s ask=%.3f buy=%.3f cost=$%.2f status=%s %s\n",
                                    mv * 100, up ? "Up" : "Down", ask, buy_px, notional, st.c_str(),
                                    (st == "REJECTED" || st == "ERROR") ? "!! NOT FILLED — NOT A POSITION" : "");
                    } else {
                        std::printf("[SNIPE-DRY] mv=%+.3f%% fav=%s ask=%.3f buy=%.3f size=%.0f cost=$%.2f tau=%.0fs deployed=$%.2f\n",
                                    mv * 100, up ? "Up" : "Down", ask, buy_px, SHARES, notional, tau, deployed);
                    }
                }
            }
        }
        if (t - last_hb > 12000) {
            const double pn = okx_now(), pa = okx_ago(3000);
            double ua, da;
            { std::lock_guard<std::mutex> lk(book_mx); ua = g_up_ask; da = g_dn_ask; }
            std::printf("[HB] okx_now=%.1f mv3s=%+.4f%% up_ask=%.3f dn_ask=%.3f\n",
                        pn, (pa > 0 ? (pn / pa - 1) * 100 : 0.0), ua, da);
            last_hb = t;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // busy-poll ~1ms
    }
    std::printf("DONE: %d snipes (cap %d)\n", trades, MAX_TRADES);
    g_run.store(false);
    to.join();
    tb.join();
    if (tbin.joinable()) tbin.join();
    return 0;
}
