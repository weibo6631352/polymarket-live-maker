// pmm/inflight.hpp — 在途成交记账 (lag-aware execution accounting)
//
// 根因: PM 链上/data-api 反映成交有 ~25s 结算延迟 → 瞬时链上查询在刚成交后是错的 (低估刚买的仓 →
// 净值假跌假急停; 刚买的份额卖不出"余额不足")。本组件跟踪"机器人已成交但链上尚未反映"的在途量,
// 提供"相信的持仓 = 链上(已结算) + 在途(未结算)", 在延迟窗口内给出正确视图。
//
// 关键不变量: 机器人持仓只因自己的成交而变 (持仓按账户算, 外部参与者不动我们的仓) → 链上位置的每次
// 变动都是我们自己成交的结算 → 链上移动了 D 就退役最旧的、累计为 D 的在途项 (不双计)。TTL 兜底未匹配项。
#pragma once

#include <deque>
#include <map>
#include <string>

namespace pmm {

class InFlightLedger {
public:
    // 在途项老化时限 (秒): 超过此龄仍未被链上匹配的在途项强制退役 (失败单/外部/匹配漏网的兜底, 防泄漏)。
    // 取 ~2× 实测结算延迟 (25s) 留余量。
    static constexpr double kTtlSeconds = 45.0;
    static constexpr double kEps = 0.5;  // 份额匹配容差 (整数股, 0.5 足够)

    struct Entry {
        double signed_size{0.0};  // +买 / -卖 (份额)
        double price{0.0};        // 成交价 (算 $ 价值)
        double acked_mono{0.0};   // 收到该成交 (ack/poll_fills) 的单调时刻
    };

    // 记录一笔机器人自己的成交 (来自订单 ack 或 poll_fills)。signed_size: +买/-卖。
    void on_fill(const std::string& token, double signed_size, double price, double now_mono);

    // 用新鲜链上持仓快照对账: 链上较上次移动多少 = 结算了多少在途 → 退役最旧的对应在途项; 再按 TTL 老化。
    void reconcile(const std::map<std::string, double>& chain_positions, double now_mono);

    // 某 token 的在途净份额 (Σ signed_size)。
    [[nodiscard]] double believed_delta(const std::string& token) const;

    // 全部在途的净 $ 成本 (Σ signed_size×price): 买 +、卖 −。供净值急停加到 (USDC + 链上市值) 上。
    [[nodiscard]] double net_cost() const;

    // 相信的持仓 = 链上 + 在途净份额 (逐 token)。供链上守卫判"是否还持有该池任一腿"。
    [[nodiscard]] std::map<std::string, double> believed_positions(
        const std::map<std::string, double>& chain_positions) const;

    // 诊断: 当前在途项总数。
    [[nodiscard]] std::size_t size() const;

private:
    // 退役 token 最旧的、累计签名量为 moved 的在途项 (链上刚结算了 moved)。
    void retire_oldest(const std::string& token, double moved);

    std::map<std::string, std::deque<Entry>> by_token_;
    std::map<std::string, double> prev_chain_;  // 上次对账的链上位置 (算移动量)
};

}  // namespace pmm
