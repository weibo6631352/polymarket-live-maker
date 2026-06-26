// pmm/ws.hpp — 实时 CLOB WebSocket 频道 (port of pm_trader/ws.py)
//
// Market 频道: 每 token 维护实时订单簿 (book 快照 + price_change 增量) → 做市环 ~ms 反应。
// User 频道: 账户真实成交 (trade 事件) 实时入队 → 替代 REST get_trades 轮询。
//
// 纯消息处理 + 订阅构造与 socket 传输分离: 传输层 (WSS 握手/帧/重连) 调 handle_message + subscribe_msg。
// 传输层参考 sports-trader-cpp 的 OpenSSL WS 实现 (见 ws_transport)。本头实现"纯逻辑 + book 缓存 + 状态"。
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/models.hpp"

namespace pmm::net {
class WsConnection;
}

namespace pmm::ws {

constexpr char MARKET_WS[] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
constexpr char USER_WS[] = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
constexpr double PING_INTERVAL_S = 10.0;

// ---- Market 频道: 实时订单簿来源 (engine book_source 接口) ----
class MarketChannel {
public:
    using PriceCallback = std::function<void(const std::string& token, double mid)>;

    // fn(token_id, mid) 在每次 mid 更新时(脱锁)触发; runner 的 reflex 用来快撤。
    void set_price_callback(PriceCallback fn);

    // 订阅消息 (纯)。
    [[nodiscard]] static std::string subscribe_msg(const std::vector<std::string>& tokens);

    // 解析一帧 (JSON 事件或数组; 非 JSON 如 PONG 只刷新存活时钟)。
    void handle_message(const std::string& raw);

    // ---- engine book-source 接口 ----
    [[nodiscard]] std::optional<OrderBook> get_book(const std::string& token_id);
    [[nodiscard]] double get_midpoint(const std::string& token_id);
    [[nodiscard]] bool fresh(const std::string& token_id, double max_age_s = 5.0);
    [[nodiscard]] int updates(const std::string& token_id);
    [[nodiscard]] bool is_live(double max_silence_s = 15.0);
    // 用权威 REST /book 快照 (RewardsClient::book 的 json) 覆盖缓存; 空/单边忽略。
    void apply_rest_snapshot(const std::string& token_id, const nlohmann::json& book);

    // 订阅集管理。
    void set_tokens(const std::vector<std::string>& tokens);
    [[nodiscard]] std::vector<std::string> tokens();  // 当前订阅集 (传输层 resubscribe 用)

    // ---- 生命周期 (传输层; 见 ws.cpp) ----
    MarketChannel() = default;
    ~MarketChannel();  // stop() reader (防 ~std::thread 在 joinable 时 terminate)
    MarketChannel(const MarketChannel&) = delete;
    MarketChannel& operator=(const MarketChannel&) = delete;
    void start();
    void stop();

private:
    std::string on_book(const nlohmann::json& ev);          // 返回 touched token ("")
    std::set<std::string> on_price_change(const nlohmann::json& ev);

    struct Levels {
        std::map<double, double> bids;
        std::map<double, double> asks;
    };

    std::mutex mu_;
    std::map<std::string, Levels> levels_;  // token -> book
    std::map<std::string, double> ts_;       // token -> last update (monotonic s)
    std::set<std::string> subs_;             // desired subscription set
    std::map<std::string, int> updates_;     // token -> cumulative book updates
    std::atomic<double> last_recv_{0.0};     // last frame time (incl PONG)
    PriceCallback on_price_;
    // 传输层: 单读线程 (WsConnection over OpenSSL)。
    void run();
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::mutex conn_mu_;                       // 保护 active_conn_ (run 注册, stop 中断)
    net::WsConnection* active_conn_{nullptr};  // 当前活连接 (stop() 用来 ::shutdown 中断 reader)
};

// ---- User 频道: 真实成交流 ----
class UserChannel {
public:
    struct Creds {
        std::string api_key;
        std::string secret;
        std::string passphrase;
    };

    UserChannel(Creds creds, bool invert_side = false);
    ~UserChannel();  // stop() reader
    UserChannel(const UserChannel&) = delete;
    UserChannel& operator=(const UserChannel&) = delete;

    [[nodiscard]] static std::string subscribe_msg(const Creds& creds,
                                                   const std::vector<std::string>& markets);
    void handle_message(const std::string& raw);
    // 排空并返回自上次以来的成交 (与 ClobSubmitter::poll_fills 同形态: {id,token_id,side,size,price})。
    [[nodiscard]] std::vector<nlohmann::json> poll_fills();
    void set_markets(const std::vector<std::string>& condition_ids);
    [[nodiscard]] std::vector<std::string> markets();

    void start();
    void stop();

private:
    Creds creds_;
    bool invert_;
    std::mutex mu_;
    std::deque<nlohmann::json> queue_;
    std::set<std::string> seen_;          // 已入队 trade id (去重查找)
    std::deque<std::string> seen_fifo_;   // 插入序 (上限回收时按"最旧"淘汰, 真 FIFO)
    std::set<std::string> markets_;
    void run();
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::mutex conn_mu_;
    net::WsConnection* active_conn_{nullptr};
};

}  // namespace pmm::ws
