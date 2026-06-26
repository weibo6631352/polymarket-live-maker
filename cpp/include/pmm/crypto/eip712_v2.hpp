// include/pmm/crypto/eip712_v2.hpp — Polymarket CTF Exchange V2 EIP-712 编码 (生产)
//
// Owner: GM (老雷) — 2026-05-31 固化自 experiments/laosun-laoli-live-order (首笔实盘成交验证)
//
// 背景: Polymarket ~2026-04-28 切到 CTF Exchange V2, V1 签名一律 order_version_mismatch 拒。
//   本模块固化已对真实 CLOB 验证通过的 V2 EIP-712 digest 编码 (digest 与官方 eth_account 逐字节一致)。
//   权威源: GitHub Polymarket/py-clob-client-v2 (旧 py-clob-client 已归档)。
//
// 范围: 纯 digest 编码 (keccak256 + EIP-712 struct/domain), 无外部依赖。
//   secp256k1 签名见 [[eip712_v2_sign]] (后续 increment), L2 HMAC + POST 见 live_pm_client。
//
// 红线: 本模块不碰私钥, 只算 digest。R-12: 不在 hot path (下单链路非 WSS event loop)。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pmm::crypto {

using Bytes32 = std::array<std::uint8_t, 32>;
using Address = std::array<std::uint8_t, 20>;

// Ethereum Keccak-256 (padding 0x01, 非 FIPS SHA3 的 0x06)。
// 测试向量: Keccak256("") == c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470。
[[nodiscard]] Bytes32 Keccak256(const std::uint8_t* data, std::size_t len) noexcept;
[[nodiscard]] inline Bytes32 Keccak256(std::string_view s) noexcept {
    return Keccak256(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// EIP-712 domain (CTF Exchange)。
struct Eip712Domain {
    std::string name;
    std::string version;
    std::uint64_t chain_id{137};
    Address verifying_contract{};
};

// CTF Exchange V2 domain (chainId 137)。neg_risk=false → exchange_v2 0xE111…; true → 0xe2222…。
[[nodiscard]] Eip712Domain CtfExchangeV2Domain(bool neg_risk = false) noexcept;

// V2 Order 的 11 个 EIP-712 字段 (权威源 py-clob-client-v2 ctf_exchange_v2_typed_data)。
//   去掉 V1 的 taker/expiration/nonce/feeRateBps; 新增 timestamp(ms)/metadata/builder。
struct OrderV2 {
    Bytes32 salt{};                  // uint256 big-endian
    Address maker{};                 // funder (proxy/Safe)
    Address signer{};                // EOA (实际签名地址)
    Bytes32 token_id{};              // uint256 big-endian (ERC1155 outcome token)
    std::uint64_t maker_amount{0};   // micro (1e6)
    std::uint64_t taker_amount{0};   // micro (1e6)
    std::uint8_t side{0};            // 0=BUY 1=SELL
    std::uint8_t signature_type{1};  // 0=EOA 1=POLY_PROXY 2=GNOSIS_SAFE 3=POLY_1271
    std::uint64_t timestamp_ms{0};   // V2 用毫秒时间戳替代 nonce
    Bytes32 metadata{};              // bytes32 (默认 0)
    Bytes32 builder{};               // bytes32 (默认 0)
};

// 计算 EIP-712 digest = keccak256(0x1901 || domainSeparator || structHash)。
// 用此 digest 走 secp256k1 ECDSA 签名。
[[nodiscard]] Bytes32 ComputeOrderV2Digest(const OrderV2& order, const Eip712Domain& domain) noexcept;

// 辅助: uint64 → 32B big-endian (abi.encode uint256)。
[[nodiscard]] Bytes32 U256FromU64(std::uint64_t v) noexcept;

// 辅助: 十进制字符串 → 32B big-endian uint256 (tokenId 可达 77 位)。溢出/非法返回 false。
[[nodiscard]] bool U256FromDecimal(std::string_view dec, Bytes32& out) noexcept;

// 辅助: "0x.." hex → Address(20B) / Bytes32(32B)。长度/字符非法返回 false。
[[nodiscard]] bool AddressFromHex(std::string_view hex, Address& out) noexcept;
[[nodiscard]] bool Bytes32FromHex(std::string_view hex, Bytes32& out) noexcept;

}  // namespace pmm::crypto
