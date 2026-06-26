// pmm/maker_live.hpp — live 做市报价基建 (port of pm_trader/maker_live.py)
//
// 从验证过的 PAPER 策略到真实 CLOB 做市的桥: 算两边报价 + mid 移动时的撤/重报 (快撤降逆向选择)。
// 安全: dry_run=true 默认, 只算不发; 真实下单走 ClobSubmitter (见 clob_submitter.hpp, 硬闸)。
//
// 本头: 纯策略 (compute_two_sided_quotes / plan_requote) + LiveMakerBot 状态机 + DryRunSubmitter +
//        ConnectionWarmer。submitter 是 callable (action json)->json, 与 dry-run echo 同接口。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/models.hpp"
#include "pmm/submitter.hpp"

namespace pmm::maker {

// submitter: (action json) -> response json。
using Submitter = std::function<nlohmann::json(const nlohmann::json&)>;

struct Order {
    std::string side;  // "BUY" / "SELL"
    double price{0.0};
    double size{0.0};
};

// 一次 poll 的计划 (对应 Python plan()/step() 返回的 dict)。
struct MakerPlan {
    std::string token_id;
    double mid{0.0};
    bool requote{false};
    std::vector<Order> orders;
    double est_reward_share{0.0};
    double committed_capital{0.0};
    double inventory{0.0};
    double skew_ticks{0.0};
    bool dry_run{false};
    bool halted{false};
    std::vector<nlohmann::json> submitted;
    std::string recommend;  // "" / "exit_cooldown"
};

// 两边报价 (YES bid + YES ask), 各离 mid half_spread_c 分, 取整到 tick, 夹到 [tick, 1-tick]。
// skew_ticks 把两边同向平移以把库存做回 flat。mid∉(0,1) 或 half_spread 越界则抛 invalid_argument。
[[nodiscard]] std::vector<Order> compute_two_sided_quotes(double mid, double half_spread_c, double size,
                                                          double tick, double max_spread_c,
                                                          double skew_ticks = 0.0);

// mid 是否移动到值得撤单重定心 (>= 一个 tick)。
[[nodiscard]] bool plan_requote(double mid_prev, double mid_now, double half_spread_c, double tick);

// GTD "dead-man" 订单的 unix 秒过期 (下限 60s, PM 拒 ~1min 内的 GTD)。
[[nodiscard]] std::int64_t gtd_expiration(double expiry_s, double now_unix);

struct MakerBotConfig {
    std::string token_id;
    double max_spread_c{0.0};
    double min_size{0.0};
    double tick{0.0};
    std::optional<double> half_spread_c;   // None -> tick*100
    std::optional<double> size;            // None -> min_size
    bool dry_run{true};
    std::optional<double> max_inventory;   // None -> 5*size
    double skew_strength_ticks{2.0};
    std::optional<double> jump_exit_ticks; // None -> max_spread_c/(tick*100)
    bool external_fills{false};
};

class LiveMakerBot {
public:
    LiveMakerBot(const MakerBotConfig& cfg, Submitter submitter = nullptr);

    [[nodiscard]] MakerPlan plan(const OrderBook& book, double mid);
    MakerPlan step(const OrderBook& book, double mid);

    // 真实成交反馈 (live external_fills): BUY→long, SELL→short, 夹到 ±max_inventory。
    void apply_real_fill(const std::string& side, double size);

    [[nodiscard]] double inventory() const noexcept { return inventory_; }
    [[nodiscard]] bool halted() const noexcept { return halted_; }
    [[nodiscard]] double size() const noexcept { return size_; }
    [[nodiscard]] double half_spread_c() const noexcept { return half_spread_c_; }
    void set_halted(bool h) noexcept { halted_ = h; }

private:
    [[nodiscard]] double skew_ticks() const;
    void detect_fill(double mid);
    [[nodiscard]] MakerPlan halt_plan(double mid, std::vector<nlohmann::json> submitted) const;

    std::string token_id_;
    bool external_fills_;
    double max_spread_c_;
    double min_size_;
    double tick_;
    double half_spread_c_;
    double size_;
    bool dry_run_;
    Submitter submitter_;
    std::optional<double> last_mid_;
    double jump_exit_ticks_;
    bool halted_{false};
    double max_inventory_;
    double skew_strength_ticks_;
    double inventory_{0.0};
    std::optional<double> last_bid_;
    std::optional<double> last_ask_;
};

// 记录每条本应发送的订单但什么都不发 (dry-live 演练 live 代码路径)。
class DryRunSubmitter : public ISubmitter {
public:
    explicit DryRunSubmitter(bool verbose = false) : verbose_(verbose) {}

    nlohmann::json operator()(const nlohmann::json& action);
    nlohmann::json submit(const nlohmann::json& action) override { return (*this)(action); }
    std::vector<nlohmann::json> poll_fills() override { return {}; }  // dry: 无真实成交
    [[nodiscard]] std::size_t sent_count() const { return sent_.size(); }

private:
    bool verbose_;
    std::deque<nlohmann::json> sent_;  // bounded maxlen 5000
};

// 守护线程每 interval 秒调 ping 保持连接热, 让偶发的撤单不付冷 TLS 握手。
class ConnectionWarmer {
public:
    explicit ConnectionWarmer(std::function<void()> ping, double interval = 3.0)
        : ping_(std::move(ping)), interval_(interval) {}
    ~ConnectionWarmer() { stop(); }
    ConnectionWarmer(const ConnectionWarmer&) = delete;
    ConnectionWarmer& operator=(const ConnectionWarmer&) = delete;

    void start();
    void stop();
    [[nodiscard]] long ticks() const noexcept { return ticks_.load(); }

private:
    void run();

    std::function<void()> ping_;
    double interval_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_{false};
    bool started_{false};
    std::thread thread_;
    std::atomic<long> ticks_{0};
};

}  // namespace pmm::maker
