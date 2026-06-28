// pmm/chain/merge_flatten.hpp — 平仓决策 (穿盘卖出 vs 买互补+CTF merge) + 双闸执行器。
//
// 决策: bot 持 N 多头时, 比较 (a) 穿自己这边 bid 卖出的所得 vs (b) 买 N 个互补 (入其 ask) 再
//   mergePositions 赎回 N USDC 的净所得。薄盘口下 (b) 常更优 (穿盘扫损是历史 ~-$28 亏损来源)。
//
// 执行双闸 (默认全 OFF, 生产零行为变化):
//   LM_FLATTEN_VIA_MERGE=1   → 决策评估开 (仍只是 DRY: 构建+签名+打印, 不发链)
//   LM_MERGE_ARM_REAL_FUNDS=1→ 真实上链开 (须与上面同时开 + 用户监督); 否则永不 eth_sendRawTransaction
//
// 实盘账户路由 (POLYMARKET_SIGNATURE_TYPE 决定, 已构建+DRY 验证):
//   sig_type=0 (EOA)       : token 由 EOA 自持 → tx.to=CTF, tx.data=mergePositions(inner)。
//   sig_type=1 (POLY_PROXY): token 由 funder 代理钱包 0x78dE 持有 (本账户)。CTF.mergePositions 烧
//     "调用者"的 token, 故必须由代理发起。控制 EOA (0xb9c8) 直接调用 ProxyWalletFactory.proxy([{CALL,
//     CTF, 0, inner}]); 工厂按 msg.sender 路由到该 EOA 的确定性代理 (= 0x78dE), 代理再 CALL CTF →
//     mergePositions 的 caller = 代理 → 烧代理 token。✓  (外层编码见 chain/proxy_exec.hpp, 已 DRY 验证;
//     CREATE2 离线证明 EOA 0xb9c8 的代理 == funder 0x78dE。)
//   sig_type=2 (GNOSIS_SAFE): 另一套架构 (Safe execTransaction), 本仓库不支持 → RouteFor 返回 ok=false,
//     绝不误用 POLY_PROXY 工厂构造错误 tx。本账户非此类型。
//   from = EOA (签名 EOA, 即工厂路由所依据的 msg.sender) — 两种路由皆然。
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

// 一笔 merge tx 的 (to, calldata) 路由结果 (按 POLYMARKET_SIGNATURE_TYPE 选)。
struct MergeRoute {
    crypto::Address to{};                 // tx.to: sig0=CTF, sig1=ProxyWalletFactory
    std::vector<std::uint8_t> calldata;   // tx.data: sig0=inner merge, sig1=proxy([{CALL,CTF,0,inner}])
    int signature_type{0};
    bool via_proxy{false};                // sig_type==1 (外层包了一层 factory.proxy)
    bool ok{false};                       // false → 不支持 (如 sig_type==2) / 地址解析失败
    std::string reason;                   // ok=false 时的原因
};

class MergeExecutor {
public:
    MergeExecutor();  // 读双闸 flag + POLYGON_RPC_URL + funder + POLYMARKET_SIGNATURE_TYPE

    [[nodiscard]] static ArmStatus Arm();       // 读两个闸的当前状态
    [[nodiscard]] bool DecisionEnabled() const { return arm_.decision_on; }
    [[nodiscard]] int SignatureType() const { return sig_type_; }

    // 按 sig_type 选路由 (to + calldata)。sig0=EOA 直发 CTF; sig1=EOA→factory.proxy→CTF; sig2=不支持。
    [[nodiscard]] MergeRoute RouteFor(const MergeQuote& q) const;

    // 构建 + 签名一笔 merge tx (纯 DRY, 不发网络)。按 RouteFor 选 to/calldata; 路由不支持→返回 false。
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
    int sig_type_{0};  // POLYMARKET_SIGNATURE_TYPE (0=EOA, 1=POLY_PROXY, 2=GNOSIS_SAFE)
};

}  // namespace pmm::chain
