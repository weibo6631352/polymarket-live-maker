// pmm/submitter.hpp — 下单器抽象接口 (DryRunSubmitter / ClobSubmitter 共用)。
//
// runner 通过此接口统一驱动: submit(action) 是核心 callable (PLACE/CANCEL_ALL/FLATTEN);
// 其余是 live 专用 (poll_fills/usdc_balance/api_creds/list_open_orders/cancel_order/close)。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pmm {

class ISubmitter {
public:
    virtual ~ISubmitter() = default;

    // 核心: 执行一个动作 (PLACE/CANCEL_ALL/FLATTEN), 返回结果 json。永不抛入热环。
    virtual nlohmann::json submit(const nlohmann::json& action) = 0;

    // live 专用 (dry-run 默认空实现)。
    virtual std::vector<nlohmann::json> poll_fills() { return {}; }
    virtual std::optional<double> usdc_balance() { return std::nullopt; }
    // 真实奖励感知 (汇总: accrued_total + earning_markets + live_pct_markets); dry-run 默认空。
    virtual nlohmann::json query_rewards(const std::string& date = "") {
        (void)date;
        return nullptr;
    }
    virtual nlohmann::json api_creds() { return nlohmann::json::object(); }
    virtual std::vector<nlohmann::json> list_open_orders() { return {}; }
    virtual void cancel_order(const std::string& order_id) { (void)order_id; }
    virtual void close() {}
    [[nodiscard]] virtual bool invert_side() const { return false; }
};

}  // namespace pmm
