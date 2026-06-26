// include/pmm/wire/clob_wire.hpp — Polymarket CLOB V2 wire 协议 (生产)
//
// Owner: GM (老雷) 2026-05-31 — 固化自 experiments (实盘验证)。
//
// 纯函数: V2 order wire body 构造 + L2 HMAC 鉴权 + L1 ClobAuth digest。无网络, 可单测锁字节。
//   下单链路: ComputeOrderV2Digest(eip712) → SignDigest(secp256k1) → BuildOrderV2Body
//             → ComputeL2Signature → HTTP POST (Phase 3 LiveOrderSubmitter)。
//
// 红线: 不碰私钥; api_secret 只用于 HMAC, 不 log。
#pragma once

#include "pmm/crypto/eip712_v2.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace pmm::wire {

// ---- base64url (Polymarket api_secret 解码 + HMAC 输出编码; 保留 = padding) ----
[[nodiscard]] std::string Base64UrlEncode(const std::uint8_t* data, std::size_t len);
[[nodiscard]] std::string Base64UrlDecode(std::string_view in);  // 返回 raw bytes 装在 string

// ---- V2 order wire body ----
struct OrderV2Wire {
    std::uint64_t salt{0};
    std::string maker;            // 0x.. (funder)
    std::string signer;           // 0x.. (EOA)
    std::string token_id;         // decimal
    std::uint64_t maker_amount{0};
    std::uint64_t taker_amount{0};
    bool is_buy{true};            // side: true=BUY false=SELL
    std::uint32_t signature_type{1};
    std::uint64_t timestamp_ms{0};
    std::string metadata_hex{"0x0000000000000000000000000000000000000000000000000000000000000000"};
    std::string builder_hex{"0x0000000000000000000000000000000000000000000000000000000000000000"};
    std::string signature;        // 0x.. (65B)
    std::string owner;            // api_key
    std::string order_type{"FOK"};
};

// 构造 byte-exact wire body JSON (用于 L2 HMAC + POST; 二者必须同一字节串)。
// 格式: py-clob-client-v2 order_to_json_v2。
[[nodiscard]] std::string BuildOrderV2Body(const OrderV2Wire& o);

// ---- L2 HMAC 鉴权 ----
// POLY_SIGNATURE = base64url(HMAC-SHA256(base64url_decode(api_secret), ts+method+path+body))。
// path 不含 querystring。返回保留 = padding。
[[nodiscard]] std::string ComputeL2Signature(std::string_view api_secret_b64url,
                                             std::string_view timestamp,
                                             std::string_view method,
                                             std::string_view path,
                                             std::string_view body);

// ---- L1 ClobAuth digest (派生 api key, F-11) ----
// domain ClobAuthDomain/version"1"/chainId137 (无 verifyingContract);
// struct ClobAuth(address,string timestamp,uint256 nonce,string message)。
// 返回 EIP-712 digest, 走 secp256k1 签名后 GET /auth/derive-api-key。
[[nodiscard]] crypto::Bytes32 ComputeClobAuthDigest(std::string_view address_lc,
                                                    std::string_view timestamp,
                                                    std::uint64_t nonce);

}  // namespace pmm::wire
