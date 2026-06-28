// pmm/chain/merge_flatten.cpp — 平仓决策 + 双闸执行器实现。
#include "pmm/chain/merge_flatten.hpp"

#include <algorithm>
#include <cstring>

#include "pmm/chain/ctf.hpp"
#include "pmm/chain/polygon_rpc.hpp"
#include "pmm/env.hpp"

namespace pmm::chain {

namespace {

std::string to_hex(const std::uint8_t* b, std::size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s = "0x";
    for (std::size_t i = 0; i < n; ++i) {
        s += h[b[i] >> 4];
        s += h[b[i] & 0x0f];
    }
    return s;
}

// 扫盘: 从 levels (已排好序) 吃 want 股, 返回 {filled, cost/proceeds}。
std::pair<double, double> sweep(double want, const std::vector<BookLevel>& levels) {
    double filled = 0.0, cash = 0.0;
    for (const auto& lv : levels) {
        if (filled >= want) break;
        const double take = std::min(want - filled, std::max(0.0, lv.size));
        filled += take;
        cash += take * lv.price;
    }
    return {filled, cash};
}

}  // namespace

FlattenDecision EvaluateFlatten(double n_shares, const std::vector<BookLevel>& yes_bids,
                                const std::vector<BookLevel>& no_asks, double gas_usdc) {
    FlattenDecision d;
    d.n_shares = n_shares;
    d.gas_usdc = gas_usdc;
    if (n_shares <= 0.0) return d;

    const auto [sf, sp] = sweep(n_shares, yes_bids);
    d.sell_filled = sf;
    d.sell_proceeds_usdc = sp;

    const auto [cf, cc] = sweep(n_shares, no_asks);
    d.complement_filled = cf;
    d.buy_complement_cost_usdc = cc;

    // 只能 merge 两腿可配对的最小股数 (持有 n YES, 买到 cf NO) → redeem 那么多 USDC。
    d.merge_redeem_usdc = std::min(n_shares, cf);
    d.merge_net_usdc = d.merge_redeem_usdc - d.buy_complement_cost_usdc - gas_usdc;

    d.merge_cheaper = d.merge_net_usdc > d.sell_proceeds_usdc;
    return d;
}

// ---------------------------------------------------------------------------

MergeExecutor::MergeExecutor() : arm_(Arm()), rpc_url_(pmm::env::str("POLYGON_RPC_URL")) {
    funder_lc_ = pmm::env::str("POLYMARKET_FUNDER");
    for (char& c : funder_lc_) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

ArmStatus MergeExecutor::Arm() {
    ArmStatus a;
    a.decision_on = pmm::env::flag_eq("LM_FLATTEN_VIA_MERGE", "1", "0");
    a.real_funds_armed = pmm::env::flag_eq("LM_MERGE_ARM_REAL_FUNDS", "1", "0");
    return a;
}

bool MergeExecutor::BuildAndSign(const MergeQuote& q, const crypto::Bytes32& private_key,
                                 std::uint64_t nonce, std::uint64_t max_priority_fee_wei,
                                 std::uint64_t max_fee_wei, std::uint64_t gas_limit,
                                 SignedTx& out) const {
    Eip1559Tx tx;
    tx.chain_id = ctf::kPolygonChainId;
    tx.nonce = nonce;
    tx.max_priority_fee_per_gas = max_priority_fee_wei;
    tx.max_fee_per_gas = max_fee_wei;
    tx.gas_limit = gas_limit;
    if (!crypto::AddressFromHex(ctf::kCtfAddress, tx.to)) return false;
    tx.value = 0;
    tx.data = ctf::EncodeMergePositionsBinary(q.collateral, q.condition_id, q.amount);
    return SignTx(tx, private_key, out);
}

nlohmann::json MergeExecutor::MaybeMerge(const MergeQuote& q,
                                         const crypto::Bytes32& private_key) const {
    nlohmann::json r;
    r["executed"] = false;
    const auto cd = ctf::EncodeMergePositionsBinary(q.collateral, q.condition_id, q.amount);
    r["calldata"] = to_hex(cd.data(), cd.size());
    r["amount"] = q.amount;
    r["decision_on"] = arm_.decision_on;
    r["real_funds_armed"] = arm_.real_funds_armed;

    if (!arm_.decision_on) {
        r["reason"] = "LM_FLATTEN_VIA_MERGE not set (merge route disabled)";
        return r;
    }
    if (rpc_url_.empty()) {
        r["reason"] = "POLYGON_RPC_URL not set";
        return r;
    }

    // 取 nonce + gas (只读 RPC; 这步本身不花钱)。
    PolygonRpc rpc(rpc_url_);
    crypto::Address eoa{};
    if (!crypto::DeriveAddress(private_key, eoa)) {
        r["reason"] = "cannot derive EOA";
        return r;
    }
    // !! 路由警告 (见头文件): sig_type=1/2 下 token 由 funder 代理持有, EOA 直发 CTF 会销毁不到任何 token。
    //    此 from=eoa 路径只对 EOA 模式 (sig_type=0) 正确; 代理模式需先包一层代理工厂 exec (尚未构建)。
    const std::string from = to_hex(eoa.data(), 20);
    const auto nonce = rpc.TransactionCount(from);
    const auto tip = rpc.MaxPriorityFeePerGas();
    const auto gp = rpc.GasPrice();
    if (!nonce || !gp) {
        r["reason"] = "RPC nonce/gasPrice query failed";
        return r;
    }
    const std::uint64_t priority = tip.value_or(30'000'000'000ULL);  // 30 gwei 兜底
    const std::uint64_t max_fee = *gp * 2ULL + priority;             // 经典 2× base + tip
    const std::vector<std::uint8_t> data =
        ctf::EncodeMergePositionsBinary(q.collateral, q.condition_id, q.amount);
    crypto::Address to_addr{};
    (void)crypto::AddressFromHex(ctf::kCtfAddress, to_addr);
    const auto gas_est = rpc.EstimateGas(from, to_hex(to_addr.data(), 20), data);
    const std::uint64_t gas = gas_est.value_or(200'000ULL) * 12ULL / 10ULL;  // +20% 余量

    SignedTx signed_tx;
    if (!BuildAndSign(q, private_key, *nonce, priority, max_fee, gas, signed_tx)) {
        r["reason"] = "BuildAndSign failed";
        return r;
    }
    r["raw_tx"] = to_hex(signed_tx.raw.data(), signed_tx.raw.size());
    r["tx_hash"] = to_hex(signed_tx.tx_hash.data(), 32);
    r["nonce"] = *nonce;
    r["gas"] = gas;
    r["max_fee_wei"] = max_fee;

    if (!arm_.real_funds_armed) {
        // 双闸未全开: 只回 DRY raw, 绝不上链 (CRITICAL SAFETY)。
        r["reason"] = "DRY: LM_MERGE_ARM_REAL_FUNDS not set — raw tx built+signed but NOT sent";
        return r;
    }

    // ===== 双闸全开: 真实上链 (用户监督下) =====
    const RpcResult send = rpc.SendRawTransaction(signed_tx.raw);
    r["executed"] = send.ok;
    r["send_result"] = send.value;
    r["http"] = send.http;
    r["reason"] = send.ok ? "sent" : "send failed";
    return r;
}

}  // namespace pmm::chain
