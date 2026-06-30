// binance_sniper_main.cpp — DRY-LIVE crypto-micro sniper (the verified edge).
// Signal (dry-live verified, ~70% win, +EV, latency-insensitive to 600ms): stream Binance BTCUSDT
// bookTicker; on a seconds-scale move (>THRESH over ~3s) BUY the FAVORED side (Up if up) at the PM ask,
// hold to resolution. NOT a fair-value model (mine is worse than PM) — RAW Binance direction only.
// This DRY build detects + logs + simulates the fill; it places NO real orders unless double-gated LIVE.
// Hard order-count cap (the scalp-trial runaway lesson). Defaults to DRY (PM_TRADER_LIVE=0).
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

static double env_d(const char* k, double d) {
    const char* v = std::getenv(k);
    return v ? std::atof(v) : d;
}
static long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
static std::string http_get(const std::string& url) {
    const std::string cmd = "curl -s --max-time 4 '" + url + "'";
    std::string out;
    char buf[8192];
    FILE* f = popen(cmd.c_str(), "r");
    if (f == nullptr) return out;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    pclose(f);
    return out;
}
static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- Binance BTC feed (the resolution source) ----
static std::mutex g_mx;
static std::deque<std::pair<long, double>> g_btc;  // (t_ms, mid)
static std::atomic<bool> g_run{true};

static double btc_now() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_btc.empty() ? 0.0 : g_btc.back().second;
}
static double btc_ago(long ms) {
    const long tgt = now_ms() - ms;
    std::lock_guard<std::mutex> lk(g_mx);
    double v = 0.0;
    for (const auto& p : g_btc) {
        if (p.first <= tgt) v = p.second;
        else break;
    }
    return v;
}

static void binance_feed() {
    while (g_run.load()) {
        pmm::net::WsConnection ws("stream.binance.com", "/ws/btcusdt@bookTicker", 5000);
        if (!ws.connect()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::printf("[BINANCE] connected\n");
        while (g_run.load()) {
            const auto m = ws.recv();
            if (m.kind == pmm::net::WsMessage::Closed) break;
            if (m.kind != pmm::net::WsMessage::Text) continue;
            try {
                auto j = json::parse(m.text);
                if (j.contains("b") && j.contains("a")) {
                    const double mid = (std::stod(j["b"].get<std::string>()) +
                                        std::stod(j["a"].get<std::string>())) / 2.0;
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_btc.emplace_back(now_ms(), mid);
                    if (g_btc.size() > 4000) g_btc.pop_front();
                }
            } catch (...) {
            }
        }
        ws.close();
    }
}

// best ask for a token via REST /book (latency-insensitive edge -> REST is fine)
static double best_ask(const std::string& tok) {
    const std::string body = http_get("https://clob.polymarket.com/book?token_id=" + tok);
    try {
        auto j = json::parse(body);
        double best = 1e9;
        for (auto& a : j["asks"]) {
            const double p = std::stod(a["price"].get<std::string>());
            if (p < best) best = p;
        }
        return best < 1e9 ? best : 0.0;
    } catch (...) {
        return 0.0;
    }
}

struct Window {
    std::string up_tok, dn_tok;
    double end_unix{0}, open_px{0};
    bool valid{false};
};

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
            std::string tk = m.value("clobTokenIds", std::string());
            if (tk.empty()) continue;
            // parse endDate
            std::tm tmv{};
            if (sscanf(m["endDate"].get<std::string>().c_str(), "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon,
                       &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6)
                continue;
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            const double end_unix = static_cast<double>(timegm(&tmv));
            const double tau = end_unix - static_cast<double>(now);
            if (tau < 120 || tau > 300) continue;
            if (tau > best_tau) {
                auto toks = json::parse(tk);
                w.up_tok = toks[0];
                w.dn_tok = toks[1];
                w.end_unix = end_unix;
                best_tau = tau;
                w.valid = true;
            }
        }
    } catch (...) {
    }
    return w;
}

static double binance_open(double end_unix) {
    const long start_ms = static_cast<long>((end_unix - 300) * 1000);
    char url[256];
    std::snprintf(url, sizeof(url),
                  "https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime=%ld&limit=1", start_ms);
    try {
        auto kl = json::parse(http_get(url));
        return std::stod(kl[0][1].get<std::string>());
    } catch (...) {
        return 0.0;
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const double THRESH = env_d("SNIPE_THRESH", 0.0003);  // 0.03% / 3s
    const int MAX_TRADES = static_cast<int>(env_d("SNIPE_MAX_TRADES", 30));
    const double ORDER_USD = env_d("SNIPE_ORDER_USD", 2.0);
    const bool LIVE = (std::getenv("PM_TRADER_LIVE") && std::string(std::getenv("PM_TRADER_LIVE")) == "1") &&
                      (std::getenv("LM_SNIPE_ARM") && std::string(std::getenv("LM_SNIPE_ARM")) == "1");
    pmm::app::LoadDotEnv(".env", LIVE);
    std::printf("=== binance-sniper ===\nmode=%s  thresh=%.4f  max_trades=%d  order=$%.2f\n",
                LIVE ? "LIVE-REAL-MONEY" : "DRY", THRESH, MAX_TRADES, ORDER_USD);

    pmm::clob::ClobSubmitter sub;
    if (LIVE && !sub.ready()) {
        std::printf("LIVE but ClobSubmitter not ready — abort\n");
        return 1;
    }

    std::thread bt(binance_feed);

    int trades = 0;
    long last_trig = 0;
    Window w;
    long last_disc = 0;
    while (g_run.load() && trades < MAX_TRADES) {
        const long t = now_ms();
        // (re)discover a fresh window
        if (!w.valid || static_cast<double>(std::time(nullptr)) > w.end_unix - 25 || t - last_disc > 15000) {
            if (!w.valid || static_cast<double>(std::time(nullptr)) > w.end_unix - 25) {
                Window nw = discover();
                if (nw.valid) {
                    nw.open_px = binance_open(nw.end_unix);
                    if (nw.open_px > 0) {
                        w = nw;
                        std::printf("[WINDOW] up=%s.. open=%.1f end_in=%.0fs\n", w.up_tok.substr(0, 12).c_str(),
                                    w.open_px, w.end_unix - static_cast<double>(std::time(nullptr)));
                    }
                }
            }
            last_disc = t;
        }
        if (!w.valid) {
            std::this_thread::sleep_for(std::chrono::seconds(4));  // back off — no fresh window (avoid gamma spam)
            continue;
        }
        // signal: Binance seconds-move
        if (w.valid && t - last_trig > 2000) {
            const double pn = btc_now(), pa = btc_ago(3000);
            if (pn > 0 && pa > 0) {
                const double mv = pn / pa - 1.0;
                if (std::fabs(mv) > THRESH) {
                    const std::string fav = mv > 0 ? w.up_tok : w.dn_tok;
                    const double a = best_ask(fav);
                    const double tau = w.end_unix - static_cast<double>(std::time(nullptr));
                    if (a > 0.03 && a < 0.97 && tau > 20) {
                        const double size = ORDER_USD / a;
                        const long det = now_ms() - t;  // rough detect latency within loop
                        if (LIVE) {
                            const auto r = sub({{"action", "PLACE"}, {"token_id", fav}, {"side", "BUY"},
                                                {"price", a}, {"size", size}});
                            std::printf("[SNIPE-LIVE] %s\n", r.dump().c_str());
                        } else {
                            std::printf("[SNIPE-DRY] move=%+.3f%% fav=%s ask=%.3f size=%.1f tau=%.0fs (no order)\n",
                                        mv * 100, (mv > 0 ? "Up" : "Down"), a, size, tau);
                        }
                        (void)det;
                        ++trades;
                        last_trig = t;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("DONE: %d snipes (cap %d)\n", trades, MAX_TRADES);
    g_run.store(false);
    bt.join();
    return 0;
}
