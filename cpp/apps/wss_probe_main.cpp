// wss_probe_main.cpp — Phase 0 of the sniper plan: measure PM CLOB WSS behaviour empirically.
// Connects to the market channel, subscribes to a live BTC up/down token, and logs the message
// rate + inter-message gap distribution + the message format. Answers: does it push every delta
// or throttle/coalesce? (Q1) — and gives the format needed to build the live local book (Phase 1).
// Read-only, no orders, zero money.
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#include "pmm/net/ws_connection.hpp"

using json = nlohmann::json;

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

static std::string discover_token() {
    std::time_t now = std::time(nullptr);
    char iso[32];
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    const std::string body = http_get(
        std::string("https://gamma-api.polymarket.com/markets?closed=false&limit=200&order=endDate&ascending=true&end_date_min=") +
        iso);
    try {
        auto arr = json::parse(body);
        for (auto& m : arr) {
            const std::string q = m.value("question", std::string());
            const std::string ql = lower(q);
            if (ql.find("up or down") == std::string::npos || ql.find("bitcoin") == std::string::npos) continue;
            if (!m.value("acceptingOrders", false)) continue;
            const std::string tk = m.value("clobTokenIds", std::string());
            if (tk.empty()) continue;
            auto toks = json::parse(tk);
            std::printf("[DISC] %s\n", q.c_str());
            return toks[0].get<std::string>();
        }
    } catch (...) {
    }
    return std::string();
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string tok = discover_token();
    if (tok.empty()) {
        std::printf("no active BTC up/down window right now\n");
        return 1;
    }
    std::printf("token=%s...\n", tok.substr(0, 20).c_str());

    pmm::net::WsConnection ws("ws-subscriptions-clob.polymarket.com", "/ws/market", 1000);
    if (!ws.connect()) {
        std::printf("WSS connect FAILED\n");
        return 1;
    }
    std::printf("WSS connected\n");
    const std::string sub = "{\"assets_ids\":[\"" + tok + "\"],\"type\":\"market\"}";
    if (!ws.send_text(sub)) {
        std::printf("subscribe send failed\n");
        return 1;
    }
    std::printf("subscribed, logging 180s...\n");

    const long start = now_ms();
    long last_ping = start, last_hb = start, last_msg_t = 0;
    int nmsg = 0, nbook = 0, npc = 0, nother = 0;
    long min_gap = 1000000000, max_gap = 0, sum_gap = 0;
    int gaps = 0;
    while (now_ms() - start < 180000) {
        const auto m = ws.recv();
        const long t = now_ms();
        if (m.kind == pmm::net::WsMessage::Text) {
            ++nmsg;
            if (last_msg_t > 0) {
                const long g = t - last_msg_t;
                if (g < min_gap) min_gap = g;
                if (g > max_gap) max_gap = g;
                sum_gap += g;
                ++gaps;
            }
            last_msg_t = t;
            std::string type = "?";
            try {
                auto j = json::parse(m.text);
                if (j.is_array() && !j.empty())
                    type = j[0].value("event_type", j[0].value("type", std::string("?")));
                else
                    type = j.value("event_type", j.value("type", std::string("?")));
            } catch (...) {
            }
            if (type == "book")
                ++nbook;
            else if (type == "price_change")
                ++npc;
            else
                ++nother;
            if (nmsg <= 6)
                std::printf("[+%ldms] #%d type=%s len=%zu | %s\n", t - start, nmsg, type.c_str(),
                            m.text.size(), m.text.substr(0, 240).c_str());
        }
        if (t - last_ping > 9000) {
            (void)ws.send_text("PING");
            last_ping = t;
        }
        if (t - last_hb > 20000) {
            std::printf("[HB %lds] msgs=%d (book=%d pc=%d other=%d) rate=%.1f/s gap(min/avg/max ms)=%ld/%ld/%ld\n",
                        (t - start) / 1000, nmsg, nbook, npc, nother,
                        1000.0 * nmsg / static_cast<double>(t - start),
                        min_gap > 100000000 ? 0 : min_gap, gaps ? sum_gap / gaps : 0, max_gap);
            last_hb = t;
        }
    }
    std::printf("DONE: %d msgs/180s = %.1f/s | book=%d price_change=%d other=%d | gap min/avg/max=%ld/%ld/%ld ms\n",
                nmsg, nmsg / 180.0, nbook, npc, nother, min_gap > 100000000 ? 0 : min_gap,
                gaps ? sum_gap / gaps : 0, max_gap);
    return 0;
}
