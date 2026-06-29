// apps/scalp_trial_main.cpp — tiny, HARD-CAPPED real-money crypto-scalp TRIAL.
//
// Answers the one question dry analysis can't: do PM "BTC/ETH Up or Down" stale quotes
// (when Kraken/Binance spot has already moved) actually PROFIT after adverse selection?
//
// SAFETY (bounds the user's downside):
//   * SERIAL: at most ONE open position at a time — no concurrent runaway.
//   * HARD NET-LOSS KILL: cum realized <= -LM_SCALP_MAXLOSS ($30) -> stop forever.
//   * PER-ORDER cap (LM_SCALP_ORDER $2), MAX trades (LM_SCALP_MAXTRADES 40).
//   * KILL FILE (touch state/scalp.kill -> stop).
//   * DOUBLE-GATE live: real orders only if PM_TRADER_LIVE=1 AND LM_SCALP_ARM=1; else DRY.
//
//   PM_TRADER_LIVE=1 LM_SCALP_ARM=1 ./scalp-trial   (REAL, hard-capped)
//   ./scalp-trial                                    (DRY rehearsal — proves safety, no money)
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/app/dotenv.hpp"
#include "pmm/clob_submitter.hpp"
#include "pmm/net/ws_connection.hpp"

using nlohmann::json;
using clk = std::chrono::steady_clock;

namespace {

double env_d(const char* k, double def) { const char* v = std::getenv(k); return v ? std::atof(v) : def; }
int env_i(const char* k, int def) { const char* v = std::getenv(k); return v ? std::atoi(v) : def; }
bool file_exists(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
double now_unix() { return static_cast<double>(std::time(nullptr)); }
double norm_cdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// low-freq HTTP GET via curl (discovery + book poll; NOT the latency-critical order path).
std::string http_get(const std::string& url) {
    std::string cmd = "curl -s --max-time 4 '" + url + "'";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    ::pclose(p);
    return out;
}

// ---- live Kraken BTC/ETH price (real-time WS, the fast signal leg) ----
std::atomic<double> g_btc{0.0}, g_eth{0.0};
std::atomic<bool> g_run{true};

void kraken_feed() {
    while (g_run.load()) {
        pmm::net::WsConnection ws("ws.kraken.com", "/v2", 3000);
        if (!ws.connect()) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        (void)ws.send_text(
            R"({"method":"subscribe","params":{"channel":"ticker","symbol":["BTC/USD","ETH/USD"]}})");
        auto last_ping = clk::now();
        while (g_run.load()) {
            auto m = ws.recv();
            if (m.kind == pmm::net::WsMessage::Closed) break;
            if (m.kind == pmm::net::WsMessage::Text && !m.text.empty()) {
                try {
                    auto j = json::parse(m.text);
                    if (j.value("channel", "") == "ticker" && j.contains("data")) {
                        for (auto& d : j["data"]) {
                            const std::string sym = d.value("symbol", "");
                            const double last = d.value("last", 0.0);
                            if (last > 0) {
                                if (sym == "BTC/USD") g_btc.store(last);
                                else if (sym == "ETH/USD") g_eth.store(last);
                            }
                        }
                    }
                } catch (...) {}
            }
            if (clk::now() - last_ping > std::chrono::seconds(20)) {
                (void)ws.send_text(R"({"method":"ping"})");
                last_ping = clk::now();
            }
        }
        ws.close();
    }
}

// ---- the hard safety state machine (MUST be bug-free) ----
struct Safety {
    double max_loss, order_usd;
    int max_trades;
    std::string killfile;
    double cum_realized{0.0};
    int trades{0};
    bool open{false}, stopped{false};
    std::string stop_reason;
    bool can_open() {
        if (stopped || open) return false;
        if (cum_realized <= -max_loss) { stop("MAXLOSS"); return false; }
        if (trades >= max_trades) { stop("MAXTRADES"); return false; }
        if (file_exists(killfile)) { stop("KILLFILE"); return false; }
        return true;
    }
    void on_open() { open = true; ++trades; }
    void on_close(double realized) {
        cum_realized += realized;
        open = false;
        if (cum_realized <= -max_loss) stop("MAXLOSS");
    }
    void stop(const std::string& why) { if (!stopped) { stopped = true; stop_reason = why; } }
};

struct Window {
    std::string asset, up_tok, down_tok;
    double end_unix{0}, open_px{0};   // open_px captured when we first see the window live
    bool open_captured{false};
};

// discover the CURRENT live BTC/ETH up/down windows (gamma).
std::vector<Window> discover() {
    std::vector<Window> out;
    const std::string body = http_get(
        "https://gamma-api.polymarket.com/markets?closed=false&limit=1500&order=startDate&ascending=false");
    if (body.empty()) return out;
    json d;
    try { d = json::parse(body); } catch (...) { return out; }
    for (auto& m : d) {
        const std::string q = m.value("question", "");
        std::string ql = q; for (auto& c : ql) c = static_cast<char>(std::tolower(c));
        if (ql.find("up or down") == std::string::npos) continue;
        const bool is_btc = ql.find("bitcoin") != std::string::npos;
        const bool is_eth = ql.find("ethereum") != std::string::npos;
        if (!is_btc && !is_eth) continue;
        if (!m.value("acceptingOrders", false) || !m.value("active", false)) continue;
        // clobTokenIds is a JSON-encoded string array
        std::vector<std::string> toks;
        try { toks = json::parse(m.value("clobTokenIds", "[]")).get<std::vector<std::string>>(); } catch (...) {}
        if (toks.size() < 2) continue;
        // endDate ISO -> unix
        const std::string ed = m.value("endDate", "");
        std::tm tm{}; double end_unix = 0;
        if (std::sscanf(ed.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 6) {
            tm.tm_year -= 1900; tm.tm_mon -= 1;
            end_unix = static_cast<double>(timegm(&tm));
        }
        if (end_unix <= 0) continue;
        Window w;
        w.asset = is_btc ? "BTC" : "ETH";
        w.up_tok = toks[0]; w.down_tok = toks[1]; w.end_unix = end_unix;
        out.push_back(w);
    }
    return out;
}

// best ask (min) for a token via CLOB /book.
double best_ask(const std::string& tok) {
    const std::string body = http_get("https://clob.polymarket.com/book?token_id=" + tok);
    if (body.empty()) return -1;
    try {
        auto j = json::parse(body);
        double best = 1e9;
        for (auto& a : j.value("asks", json::array())) {
            double p = std::stod(a.value("price", "9"));
            if (p < best) best = p;
        }
        return best < 1e9 ? best : -1;
    } catch (...) { return -1; }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffered -> real-time log visibility (esp. live)
    Safety sf{env_d("LM_SCALP_MAXLOSS", 30.0), env_d("LM_SCALP_ORDER", 2.0),
              env_i("LM_SCALP_MAXTRADES", 40),
              std::getenv("LM_SCALP_KILLFILE") ? std::getenv("LM_SCALP_KILLFILE") : "state/scalp.kill"};
    const double sigma = env_d("LM_SCALP_SIGMA", 0.0025);   // ~5min BTC vol fraction
    const double margin = env_d("LM_SCALP_MARGIN", 0.04);   // required edge over fair (covers fee+slip)
    const double run_secs = env_d("LM_SCALP_RUNSECS", 7200);

    const bool LIVE = std::getenv("PM_TRADER_LIVE") && std::string(std::getenv("PM_TRADER_LIVE")) == "1" &&
                      std::getenv("LM_SCALP_ARM") && std::string(std::getenv("LM_SCALP_ARM")) == "1";

    std::printf("=== scalp-trial ===\nmode=%s  max_loss=$%.2f  order=$%.2f  max_trades=%d\n",
                LIVE ? "LIVE-REAL-MONEY" : "DRY", sf.max_loss, sf.order_usd, sf.max_trades);
    std::printf("sigma=%.4f margin=%.3f killfile=%s run=%.0fs\n",
                sigma, margin, sf.killfile.c_str(), run_secs);

    pmm::app::LoadDotEnv(".env", /*live_mode=*/LIVE);
    pmm::clob::ClobSubmitter sub;
    if (LIVE && !sub.ready()) { std::fprintf(stderr, "LIVE but ClobSubmitter NOT ready. Abort.\n"); return 1; }
    if (LIVE) std::printf("signer=%s\n", sub.signer_address().c_str());

    std::thread feed(kraken_feed);
    const auto t_start = clk::now();
    std::map<std::string, Window> seen;  // persistent: up_tok -> window with captured open_px
    double last_discover = 0, last_hb = 0;
    // current open position (serial): token + side-prob-at-entry + shares + cost + the window end
    struct Pos { std::string asset, tok; bool up; double shares, cost, end_unix, open_px; } pos{};

    while (!sf.stopped && std::chrono::duration<double>(clk::now() - t_start).count() < run_secs) {
        const double t = now_unix();
        // refresh windows every 20s; capture open price the first time a window is seen live
        if (t - last_discover > 15) {
            last_discover = t;
            auto found = discover();
            int newcap = 0;
            double nearest = 1e9;
            for (auto& w : found) {
                const double start = w.end_unix - 300.0;       // 5-min window
                const double dt = t - start;                   // seconds since this window's open
                if (std::abs(dt) < std::abs(nearest)) nearest = dt;
                if (seen.count(w.up_tok)) continue;            // already tracking -> keep its captured open
                if (dt >= -5 && dt < 25) {                     // caught it within ~25s of its open
                    const double px = (w.asset == "BTC") ? g_btc.load() : g_eth.load();
                    if (px > 0) { w.open_px = px; w.open_captured = true; seen[w.up_tok] = w; ++newcap; }
                }
            }
            std::printf("[DISC] found=%zu tracked=%zu newcap=%d nearest_dt=%.0fs\n",
                        found.size(), seen.size(), newcap, nearest);
            for (auto it = seen.begin(); it != seen.end();) {  // prune ended windows
                if (t > it->second.end_unix + 60) it = seen.erase(it); else ++it;
            }
        }
        // heartbeat: prove feed + discovery + fair-value are live (DRY visibility + LIVE monitoring)
        if (t - last_hb > 30) {
            last_hb = t;
            std::printf("[HB] btc=%.1f eth=%.1f tracked=%zu cum=$%.2f trades=%d%s\n",
                        g_btc.load(), g_eth.load(), seen.size(), sf.cum_realized, sf.trades,
                        sf.open ? " POS-OPEN" : "");
            for (auto& kv : seen) {
                Window& w = kv.second;
                const double tau = w.end_unix - t;
                if (tau < 0) continue;
                const double px = (w.asset == "BTC") ? g_btc.load() : g_eth.load();
                const double z = std::log(px / w.open_px) / (sigma * std::sqrt(std::max(tau, 1.0) / 300.0));
                std::printf("     %s tau=%.0fs open=%.1f now=%.1f fairP_up=%.3f\n",
                            w.asset.c_str(), tau, w.open_px, px, norm_cdf(z));
            }
        }
        // 1) if a position is open, check for resolution at window end -> realize
        if (sf.open) {
            if (t >= pos.end_unix + 2) {
                double endpx = (pos.asset == "BTC") ? g_btc.load() : g_eth.load();
                bool up_won = endpx > pos.open_px;
                bool we_won = (pos.up == up_won);
                double realized = (we_won ? pos.shares * 1.0 : 0.0) - pos.cost;  // winner->$1/share
                sf.on_close(realized);
                std::printf("[RESOLVE] %s up_won=%d we_up=%d shares=%.2f cost=$%.2f -> realized=$%.3f cum=$%.3f\n",
                            pos.asset.c_str(), up_won, pos.up, pos.shares, pos.cost, realized, sf.cum_realized);
            }
        }
        // 2) else look for a mispriced takeable window
        else if (sf.can_open()) {
            for (auto& kv : seen) {
                Window& w = kv.second;
                if (!w.open_captured || w.open_px <= 0) continue;
                double tau = w.end_unix - t;
                if (tau < 30 || tau > 290) continue;             // trade mid-window only
                double px = (w.asset == "BTC") ? g_btc.load() : g_eth.load();
                if (px <= 0) continue;
                double z = std::log(px / w.open_px) / (sigma * std::sqrt(tau / 300.0));
                double p_up = norm_cdf(z);                       // fair P(Up)
                double up_ask = best_ask(w.up_tok);
                double dn_ask = best_ask(w.down_tok);
                bool buy_up = up_ask > 0 && up_ask < p_up - margin;
                bool buy_dn = dn_ask > 0 && dn_ask < (1 - p_up) - margin;
                if (!buy_up && !buy_dn) continue;
                const std::string tok = buy_up ? w.up_tok : w.down_tok;
                double price = buy_up ? up_ask : dn_ask;
                double shares = sf.order_usd / price;
                std::printf("[SIGNAL] %s fairP_up=%.3f up_ask=%.3f dn_ask=%.3f -> BUY %s @%.3f x%.1f ($%.2f)\n",
                            w.asset.c_str(), p_up, up_ask, dn_ask, buy_up ? "UP" : "DOWN", price, shares,
                            sf.order_usd);
                bool filled = false;
                if (LIVE) {
                    auto r = sub({{"action", "PLACE"}, {"token_id", tok}, {"side", "BUY"},
                                  {"price", price}, {"size", shares}});
                    filled = r.value("success", false) || r.contains("orderID") || r.contains("orderId");
                    std::printf("[PLACE-LIVE] %s\n", r.dump().c_str());
                } else {
                    filled = true;  // DRY: assume fill at the ask
                    std::printf("[PLACE-DRY] (no real order)\n");
                }
                if (filled) {
                    sf.on_open();
                    pos = {w.asset, tok, buy_up, shares, sf.order_usd, w.end_unix, w.open_px};
                }
                break;  // serial: one at a time
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    g_run.store(false);
    if (feed.joinable()) { /* feed blocks in recv; detach to exit promptly */ feed.detach(); }
    std::printf("=== DONE: trades=%d cum_realized=$%.3f stopped=%s ===\n",
                sf.trades, sf.cum_realized, sf.stopped ? sf.stop_reason.c_str() : "(time/limit)");
    return 0;
}
