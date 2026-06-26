// include/pmm/crypto/secp256k1_signer.hpp — Ethereum ECDSA 签名 (Polymarket V2)
//
// Owner: GM (老雷) 2026-05-31 — 固化下单签名 (首笔实盘成交验证)。
//
// secp256k1 ECDSA + recover (以太坊曲线, 非 ed25519)。配合 [[eip712_v2]] digest 用:
//   ComputeOrderV2Digest → SignDigest → 65B (r||s||v) → wire body signature。
//
// 红线: 私钥只读不持有不 log。SignDigest 内部不缓存私钥。R-12: 非 hot path。
#pragma once

#include "pmm/crypto/eip712_v2.hpp"

#include <array>
#include <cstdint>

namespace pmm::crypto {

using Signature65 = std::array<std::uint8_t, 65>;  // r(32) || s(32) || v(1, =recid+27)

// 用 secp256k1 私钥对 32B digest 做可恢复 ECDSA 签名 → 65B (r||s||v)。
// RFC6979 确定性 nonce (低 s 规范化)。私钥非法返回 false。
[[nodiscard]] bool SignDigest(const Bytes32& digest, const Bytes32& private_key, Signature65& out) noexcept;

// 从 digest + 65B 签名恢复签名者地址 (20B)。失败返回 false。
// 用于下单前自检: recover 出的地址必须 == signer EOA (小白 P1-5)。
[[nodiscard]] bool RecoverAddress(const Bytes32& digest, const Signature65& sig, Address& out) noexcept;

// 私钥 → EOA 地址 (keccak256(pubkey[1..65])[12..32])。私钥非法返回 false。
[[nodiscard]] bool DeriveAddress(const Bytes32& private_key, Address& out) noexcept;

}  // namespace pmm::crypto
