// src/pmm/ws.cpp — 实时 WS 频道: 纯消息处理 + book 缓存 + 订阅状态 (port of pm_trader/ws.py)
//
// 传输层 (WSS socket loop) 待补 (参考 sports-trader-cpp OpenSSL WS); start/stop 当前占位。
#include "pmm/ws.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "pmm/jsonutil.hpp"
#include "pmm/net/ws_connection.hpp"

namespace pmm::ws {

namespace ju = pmm::jsonutil;
using nlohmann::json;

namespace {

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>((c >= 'a' && c <= 'z') ? c - 32 : c);
    return s;
}

// wss://host/path → (host, path)。
std::pair<std::string, std::string> parse_wss(const std::string& url) {
    std::string s = url;
    const std::string pfx = "wss://";
    if (s.rfind(pfx, 0) == 0) s = s.substr(pfx.size());
    const auto slash = s.find('/');
    if (slash == std::string::npos) return {s, "/"};
    return {s.substr(0, slash), s.substr(slash)};
}

// 可中断睡眠: 期间 stop 置位则提前返回 true。
bool interruptible_sleep(std::atomic<bool>& stop, double seconds) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + duration<double>(seconds);
    while (steady_clock::now() < deadline) {
        if (stop.load()) return true;
        std::this_thread::sleep_for(milliseconds(100));
    }
    return stop.load();
}

// 把 [{"price":..,"size":..}, ...] 解析成 price→size。
std::map<double, double> parse_levels(const json& arr) {
    std::map<double, double> out;
    if (arr.is_array()) {
        for (const auto& l : arr) {
            const json* p = ju::find(l, "price");
            const json* s = ju::find(l, "size");
            out[p ? ju::to_double(*p) : 0.0] = s ? ju::to_double(*s) : 0.0;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// MarketChannel
// ---------------------------------------------------------------------------

void MarketChannel::set_price_callback(PriceCallback fn) {
    on_price_ = std::move(fn);
}

std::string MarketChannel::subscribe_msg(const std::vector<std::string>& tokens) {
    return json{{"assets_ids", tokens}, {"type", "market"}, {"custom_feature_enabled", true}}.dump();
}

std::string MarketChannel::on_book(const json& ev) {
    const json* tk = ju::find(ev, "asset_id");
    if (tk == nullptr) return {};
    const std::string token = ju::to_str(*tk);
    if (token.empty()) return {};
    Levels lv;
    if (const json* b = ju::find(ev, "bids")) lv.bids = parse_levels(*b);
    if (const json* a = ju::find(ev, "asks")) lv.asks = parse_levels(*a);
    {
        std::lock_guard<std::mutex> g(mu_);
        levels_[token] = std::move(lv);  // 全量快照
        ts_[token] = mono_now();
    }
    return token;
}

std::set<std::string> MarketChannel::on_price_change(const json& ev) {
    std::set<std::string> touched;
    const json* changes = ju::find(ev, "price_changes");
    if (changes == nullptr || !changes->is_array()) return touched;
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& ch : *changes) {
        const json* tk = ju::find(ch, "asset_id");
        if (tk == nullptr) continue;
        const std::string token = ju::to_str(*tk);
        if (token.empty()) continue;
        touched.insert(token);
        Levels& book = levels_[token];
        const std::string side = (ju::find(ch, "side") && upper(ju::to_str(*ju::find(ch, "side"))) == "BUY")
                                     ? "bids"
                                     : "asks";
        const double price = ju::find(ch, "price") ? ju::to_double(*ju::find(ch, "price")) : 0.0;
        const double size = ju::find(ch, "size") ? ju::to_double(*ju::find(ch, "size")) : 0.0;
        std::map<double, double>& m = (side == "bids") ? book.bids : book.asks;
        if (size <= 0.0) {
            m.erase(price);  // size 0 → 删档
        } else {
            m[price] = size;
        }
        ts_[token] = mono_now();
    }
    return touched;
}

void MarketChannel::handle_message(const std::string& raw) {
    last_recv_.store(mono_now());  // 任意帧 (含 PONG) = 存活
    json data;
    try {
        data = json::parse(raw);
    } catch (...) {
        return;
    }
    std::set<std::string> touched;
    const auto handle_ev = [&](const json& ev) {
        if (!ev.is_object()) return;
        const json* et = ju::find(ev, "event_type");
        const std::string type = et ? ju::to_str(*et) : "";
        if (type == "book") {
            const std::string t = on_book(ev);
            if (!t.empty()) touched.insert(t);
        } else if (type == "price_change") {
            const auto tt = on_price_change(ev);
            touched.insert(tt.begin(), tt.end());
        }
    };
    if (data.is_array()) {
        for (const auto& ev : data) handle_ev(ev);
    } else {
        handle_ev(data);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& t : touched) updates_[t] += 1;
    }
    // 价格回调脱锁触发 (可能做 I/O / 拿别的锁)。
    if (on_price_) {
        std::set<std::string> subs_copy;
        {
            std::lock_guard<std::mutex> g(mu_);
            subs_copy = subs_;
        }
        for (const auto& t : touched) {
            if (subs_copy.count(t) != 0) {
                const double mid = get_midpoint(t);
                if (mid > 0.0) on_price_(t, mid);
            }
        }
    }
}

std::optional<OrderBook> MarketChannel::get_book(const std::string& token_id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = levels_.find(token_id);
    if (it == levels_.end() || it->second.bids.empty() || it->second.asks.empty()) return std::nullopt;
    OrderBook ob;
    for (const auto& [p, s] : it->second.bids) {
        if (s > 0.0) ob.bids.push_back({p, s});
    }
    for (const auto& [p, s] : it->second.asks) {
        if (s > 0.0) ob.asks.push_back({p, s});
    }
    if (ob.bids.empty() || ob.asks.empty()) return std::nullopt;
    return ob;
}

double MarketChannel::get_midpoint(const std::string& token_id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = levels_.find(token_id);
    if (it == levels_.end() || it->second.bids.empty() || it->second.asks.empty()) return 0.0;
    const double best_bid = it->second.bids.rbegin()->first;  // 最高 bid
    const double best_ask = it->second.asks.begin()->first;   // 最低 ask
    return (best_bid + best_ask) / 2.0;
}

bool MarketChannel::fresh(const std::string& token_id, double max_age_s) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = ts_.find(token_id);
    return it != ts_.end() && (mono_now() - it->second) <= max_age_s;
}

int MarketChannel::updates(const std::string& token_id) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = updates_.find(token_id);
    return it != updates_.end() ? it->second : 0;
}

bool MarketChannel::is_live(double max_silence_s) {
    const double lr = last_recv_.load();
    return lr > 0.0 && (mono_now() - lr) <= max_silence_s;
}

void MarketChannel::apply_rest_snapshot(const std::string& token_id, const json& book) {
    if (token_id.empty() || !book.is_object()) return;
    Levels lv;
    if (const json* b = ju::find(book, "bids")) lv.bids = parse_levels(*b);
    if (const json* a = ju::find(book, "asks")) lv.asks = parse_levels(*a);
    if (lv.bids.empty() || lv.asks.empty()) return;  // 不用垃圾覆盖好 book
    std::lock_guard<std::mutex> g(mu_);
    levels_[token_id] = std::move(lv);
    ts_[token_id] = mono_now();
}

void MarketChannel::set_tokens(const std::vector<std::string>& tokens) {
    std::set<std::string> new_set;
    for (const auto& t : tokens) {
        if (!t.empty()) new_set.insert(t);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        subs_ = new_set;
        for (auto it = levels_.begin(); it != levels_.end();) {  // 丢掉不再需要的 book
            if (new_set.count(it->first) == 0) {
                ts_.erase(it->first);
                it = levels_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // TODO(transport): resubscribe 当连接活着。
}

std::vector<std::string> MarketChannel::tokens() {
    std::lock_guard<std::mutex> g(mu_);
    return {subs_.begin(), subs_.end()};
}

void MarketChannel::start() {
    if (reader_.joinable()) return;
    stop_.store(false);
    reader_ = std::thread([this] { run(); });
}

void MarketChannel::stop() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lk(conn_mu_);
        if (active_conn_ != nullptr) active_conn_->shutdown_socket();  // 立即中断阻塞的 recv
    }
    if (reader_.joinable()) reader_.join();
}

MarketChannel::~MarketChannel() { stop(); }

void MarketChannel::run() {
    const auto [host, path] = parse_wss(MARKET_WS);
    while (!stop_.load()) {
        std::vector<std::string> toks = tokens();
        if (toks.empty()) {  // 无订阅 → 不持空闲连接
            if (interruptible_sleep(stop_, 0.5)) break;
            continue;
        }
        pmm::net::WsConnection conn(host, path, 2000);
        if (!conn.connect()) {
            if (interruptible_sleep(stop_, 2.0)) break;  // 重连退避
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(conn_mu_);
            active_conn_ = &conn;
        }
        std::vector<std::string> subscribed = toks;
        (void)conn.send_text(subscribe_msg(subscribed));
        double last_ping = mono_now();
        while (!stop_.load()) {
            std::vector<std::string> cur = tokens();
            if (cur.empty()) break;  // 全退订 → 断连
            if (cur != subscribed) {  // 订阅集变化 → 重订
                (void)conn.send_text(subscribe_msg(cur));
                subscribed = cur;
            }
            const double now = mono_now();
            if (now - last_ping >= PING_INTERVAL_S) {
                (void)conn.send_text("PING");  // 文本 PING (PM 回文本 PONG)
                last_ping = now;
            }
            const pmm::net::WsMessage msg = conn.recv();
            if (msg.kind == pmm::net::WsMessage::Closed) break;
            if (msg.kind == pmm::net::WsMessage::Text && !msg.text.empty()) handle_message(msg.text);
        }
        {
            std::lock_guard<std::mutex> lk(conn_mu_);
            active_conn_ = nullptr;
        }
        conn.close();
        if (!stop_.load() && interruptible_sleep(stop_, 2.0)) break;
    }
}

// ---------------------------------------------------------------------------
// UserChannel
// ---------------------------------------------------------------------------

UserChannel::UserChannel(Creds creds, bool invert_side)
    : creds_(std::move(creds)), invert_(invert_side) {}

std::string UserChannel::subscribe_msg(const Creds& creds, const std::vector<std::string>& markets) {
    return json{{"type", "user"},
                {"auth",
                 {{"apiKey", creds.api_key}, {"secret", creds.secret}, {"passphrase", creds.passphrase}}},
                {"markets", markets}}
        .dump();
}

void UserChannel::handle_message(const std::string& raw) {
    json data;
    try {
        data = json::parse(raw);
    } catch (...) {
        return;
    }
    const auto handle_ev = [&](const json& ev) {
        if (!ev.is_object()) return;
        const json* et = ju::find(ev, "event_type");
        if (et == nullptr || ju::to_str(*et) != "trade") return;
        const json* idp = ju::find(ev, "id");
        const std::string tid = idp ? ju::to_str(*idp) : "";
        std::lock_guard<std::mutex> g(mu_);
        if (!tid.empty()) {
            if (seen_.count(tid) != 0) return;  // 去重
            seen_.insert(tid);
            seen_fifo_.push_back(tid);
            if (seen_fifo_.size() > 5000) {  // 真 FIFO: 淘汰最旧, 绝不丢刚插入的 (防重复成交被放行)
                seen_.erase(seen_fifo_.front());
                seen_fifo_.pop_front();
            }
        }
        std::string side = upper(ju::find(ev, "side") ? ju::to_str(*ju::find(ev, "side")) : "");
        if (invert_) side = (side == "BUY") ? "SELL" : "BUY";
        json f;
        f["id"] = tid;
        f["token_id"] = ju::find(ev, "asset_id") ? ju::to_str(*ju::find(ev, "asset_id")) : "";
        f["side"] = side;
        f["size"] = ju::find(ev, "size") ? ju::to_double(*ju::find(ev, "size")) : 0.0;
        f["price"] = ju::find(ev, "price") ? ju::to_double(*ju::find(ev, "price")) : 0.0;
        queue_.push_back(std::move(f));
    };
    if (data.is_array()) {
        for (const auto& ev : data) handle_ev(ev);
    } else {
        handle_ev(data);
    }
}

std::vector<json> UserChannel::poll_fills() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<json> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

void UserChannel::set_markets(const std::vector<std::string>& condition_ids) {
    std::set<std::string> m;
    for (const auto& c : condition_ids) {
        if (!c.empty()) m.insert(c);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        markets_ = std::move(m);
    }
    // TODO(transport): resubscribe。
}

std::vector<std::string> UserChannel::markets() {
    std::lock_guard<std::mutex> g(mu_);
    return {markets_.begin(), markets_.end()};
}

void UserChannel::start() {
    if (reader_.joinable()) return;
    stop_.store(false);
    reader_ = std::thread([this] { run(); });
}

void UserChannel::stop() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lk(conn_mu_);
        if (active_conn_ != nullptr) active_conn_->shutdown_socket();
    }
    if (reader_.joinable()) reader_.join();
}

UserChannel::~UserChannel() { stop(); }

void UserChannel::run() {
    const auto [host, path] = parse_wss(USER_WS);
    while (!stop_.load()) {
        std::vector<std::string> mk = markets();
        if (mk.empty()) {
            if (interruptible_sleep(stop_, 0.5)) break;
            continue;
        }
        pmm::net::WsConnection conn(host, path, 2000);
        if (!conn.connect()) {
            if (interruptible_sleep(stop_, 2.0)) break;
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(conn_mu_);
            active_conn_ = &conn;
        }
        std::vector<std::string> subscribed = mk;
        (void)conn.send_text(subscribe_msg(creds_, subscribed));
        double last_ping = mono_now();
        while (!stop_.load()) {
            std::vector<std::string> cur = markets();
            if (cur.empty()) break;
            if (cur != subscribed) {
                (void)conn.send_text(subscribe_msg(creds_, cur));
                subscribed = cur;
            }
            const double now = mono_now();
            if (now - last_ping >= PING_INTERVAL_S) {
                (void)conn.send_text("PING");
                last_ping = now;
            }
            const pmm::net::WsMessage msg = conn.recv();
            if (msg.kind == pmm::net::WsMessage::Closed) break;
            if (msg.kind == pmm::net::WsMessage::Text && !msg.text.empty()) handle_message(msg.text);
        }
        {
            std::lock_guard<std::mutex> lk(conn_mu_);
            active_conn_ = nullptr;
        }
        conn.close();
        if (!stop_.load() && interruptible_sleep(stop_, 2.0)) break;
    }
}

}  // namespace pmm::ws
