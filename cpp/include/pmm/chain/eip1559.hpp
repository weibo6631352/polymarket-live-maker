// pmm/chain/eip1559.hpp — EIP-1559 (type-2) Polygon 交易构建 + 签名。
//
// preimage = 0x02 || rlp([chainId, nonce, maxPriorityFee, maxFee, gas, to, value, data, accessList=[]])
// sighash  = keccak256(preimage); 用现有 secp256k1 signer 签 → (r, s, yParity)
// raw      = 0x02 || rlp([... , yParity, r, s])
//
// 复用项目已实盘验证的 crypto: Keccak256 + SignDigest (低 s 规范化) + RecoverAddress 自检。
// DRY 验证: test_merge_encode 用确定性测试私钥构建一笔 merge tx, raw 与 eth_account 逐字节匹配。
//
// 红线: 本模块只构建+签名 raw 字节, 不发网络 (发送在 polygon_rpc, 双闸默认 OFF)。R-12: 非 hot path。
#pragma once

#include <cstdint>
#include <vector>

#include "pmm/crypto/eip712_v2.hpp"       // crypto::Address / Bytes32 / Keccak256
#include "pmm/crypto/secp256k1_signer.hpp"

namespace pmm::chain {

// EIP-1559 交易字段 (accessList 固定空 — merge 调用无需)。fee/value 单位 wei。
struct Eip1559Tx {
    std::uint64_t chain_id{137};
    std::uint64_t nonce{0};
    std::uint64_t max_priority_fee_per_gas{0};  // wei
    std::uint64_t max_fee_per_gas{0};           // wei
    std::uint64_t gas_limit{0};
    crypto::Address to{};
    std::uint64_t value{0};  // wei (merge 调用 = 0)
    std::vector<std::uint8_t> data;
};

// 未签名 preimage = 0x02 || rlp([9 字段])。
[[nodiscard]] std::vector<std::uint8_t> EncodeUnsigned(const Eip1559Tx& tx);

// 待签 digest = keccak256(preimage)。
[[nodiscard]] crypto::Bytes32 SigningHash(const Eip1559Tx& tx);

struct SignedTx {
    std::vector<std::uint8_t> raw;  // 0x02 || rlp([..., yParity, r, s]) — 可直接 eth_sendRawTransaction
    crypto::Bytes32 tx_hash{};      // keccak256(raw)
    std::uint8_t y_parity{0};       // 0/1
};

// 用私钥签 tx → SignedTx。内部做 recover 自检 (恢复地址须 == 私钥派生 EOA), 失败返回 false。
[[nodiscard]] bool SignTx(const Eip1559Tx& tx, const crypto::Bytes32& private_key, SignedTx& out);

}  // namespace pmm::chain
