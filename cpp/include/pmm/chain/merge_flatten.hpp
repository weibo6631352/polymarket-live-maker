// pmm/chain/merge_flatten.hpp — 平仓决策 (穿盘卖出 vs 买互补+CTF merge) + 双闸执行器。
//
// 决策: bot 持 N 多头时, 比较 (a) 穿自己这边 bid 卖出的所得 vs (b) 买 N 个互补 (入其 ask) 再
//   mergePositions 赎回 N USDC 的净所得。薄盘口下 (b) 常更优 (穿盘扫损是历史 ~-$28 亏损来源)。
//
// 执行双闸 (默认全 OFF, 生产零行为变化):
//   LM_FLATTEN_VIA_MERGE=1   → 决策评估开 (仍只是 DRY: 构建+签名+打印, 不发链)
//   LM_MERGE_ARM_REAL_FUNDS=1→ 真实上链开 (须与上面同时开 + 用户监督); 否则永不 eth_sendRawTransaction
//
// 红线: 私钥只读不持有不 log。R-12: 非 hot path。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pmm/chain/eip1559.hpp"
#include "pmm/crypto/eip712_v2.hpp"

namespace pmm::chain {

struct BookLevel {
    double price{0.0};
    double size{0.0};
};

// 平掉 N 个多头的成本对比结果。
struct FlattenDecision {
    double n_shares{0.0};
    double sell_proceeds_usdc{0.0};        // 穿 bid 卖 N 得到的 USDC
    double sell_filled{0.0};               // bid 盘能吃下的股数
    double buy_complement_cost_usdc{0.0};  // 穿 ask 买 N 互补的花费
    double complement_filled{0.0};         // ask 盘能给出的股数
    double merge_redeem_usdc{0.0};         // merge 赎回 USDC (= 两腿可配对的最小股数)
    double merge_net_usdc{0.0};            // redeem - buy_cost - gas
    double gas_usdc{0.0};
    bool merge_cheaper{false};  // merge_net_usdc > sell_proceeds_usdc
};

// 评估: yes_bids 应按价降序 (自己这边盘口), no_asks 按价升序 (互补盘口)。gas_usdc = 估算的链上 gas (USDC)。
[[nodiscard]] FlattenDecision EvaluateFlatten(double n_shares, const std::vector<BookLevel>& yes_bids,
                                              const std::vector<BookLevel>& no_asks, double gas_usdc);

// merge 一笔的参数 (二元市场)。
struct MergeQuote {
    crypto::Address collateral{};   // 抵押 (USDC.e)
    crypto::Bytes32 condition_id{}; // 市场 conditionId
    std::uint64_t amount{0};        // 抵押最小单位 (shares × 1e6)
};

struct ArmStatus {
    bool decision_on{false};      // LM_FLATTEN_VIA_MERGE
    bool real_funds_armed{false}; // LM_MERGE_ARM_REAL_FUNDS
};

class MergeExecutor {
public:
    MergeExecutor();  // 读双闸 flag + POLYGON_RPC_URL + funder (POLYMARKET_FUNDER)

    [[nodiscard]] static ArmStatus Arm();       // 读两个闸的当前状态
    [[nodiscard]] bool DecisionEnabled() const { return arm_.decision_on; }

    // 构建 + 签名一笔 merge tx (纯 DRY, 不发网络)。可单测 (raw 逐字节匹配 eth_account)。
    [[nodiscard]] bool BuildAndSign(const MergeQuote& q, const crypto::Bytes32& private_key,
                                    std::uint64_t nonce, std::uint64_t max_priority_fee_wei,
                                    std::uint64_t max_fee_wei, std::uint64_t gas_limit,
                                    SignedTx& out) const;

    // 编排: 若双闸全开 + RPC 可用 → 查 nonce/gas → 签 → eth_sendRawTransaction (真实上链)。
    //   否则 (默认) → executed=false, 仅返回会发送的 raw tx + 原因, 供用户监督审查。绝不在双闸未全开时发链。
    [[nodiscard]] nlohmann::json MaybeMerge(const MergeQuote& q,
                                            const crypto::Bytes32& private_key) const;

private:
    ArmStatus arm_;
    std::string rpc_url_;
    std::string funder_lc_;
};

}  // namespace pmm::chain
