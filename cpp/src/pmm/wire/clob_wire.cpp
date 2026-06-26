// src/pmm/polymarket/wire/clob_wire.cpp — Polymarket CLOB V2 wire 协议 (生产)
//
// Owner: GM (老雷) 2026-05-31。固化自 experiments/laosun-laoli-live-order (实盘验证)。
#include "pmm/wire/clob_wire.hpp"

#include <cstring>

// 2026-06-12 治理: HMAC-SHA256 由 libsodium 换 OpenSSL (项目唯一 crypto/TLS 外部依赖,
//   WSS 已用)。换装由 test_clob_wire L2 HMAC 向量 (与 python hmac 对齐) 验证。
#include <openssl/hmac.h>

namespace pmm::wire {

namespace {

constexpr char kB64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int B64UrlVal(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// abi.encode(address) → 32B (左填 12 零)。address_lc 形如 "0x" + 40 hex。
crypto::Bytes32 Addr32(std::string_view address_lc) noexcept {
    crypto::Bytes32 out{};
    crypto::Address a{};
    if (crypto::AddressFromHex(address_lc, a))
        std::memcpy(out.data() + 12, a.data(), 20);
    return out;
}

}  // namespace

std::string Base64UrlEncode(const std::uint8_t* data, std::size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = (i + 1 < len) ? data[i + 1] : 0;
        const unsigned b2 = (i + 2 < len) ? data[i + 2] : 0;
        out.push_back(kB64UrlAlphabet[b0 >> 2]);
        out.push_back(kB64UrlAlphabet[((b0 & 3) << 4) | (b1 >> 4)]);
        out.push_back((i + 1 < len) ? kB64UrlAlphabet[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        out.push_back((i + 2 < len) ? kB64UrlAlphabet[b2 & 63] : '=');
    }
    return out;
}

std::string Base64UrlDecode(std::string_view in) {
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = B64UrlVal(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xff));
        }
    }
    return out;
}

std::string BuildOrderV2Body(const OrderV2Wire& o) {
    // 字节序须与 L2 HMAC 一致 (同一字符串既算 HMAC 又 POST)。
    std::string s;
    s.reserve(640);
    s += "{\"order\":{";
    s += "\"salt\":" + std::to_string(o.salt) + ",";
    s += "\"maker\":\"" + o.maker + "\",";
    s += "\"signer\":\"" + o.signer + "\",";
    s += "\"tokenId\":\"" + o.token_id + "\",";
    s += "\"makerAmount\":\"" + std::to_string(o.maker_amount) + "\",";
    s += "\"takerAmount\":\"" + std::to_string(o.taker_amount) + "\",";
    s += o.is_buy ? "\"side\":\"BUY\"," : "\"side\":\"SELL\",";
    s += "\"expiration\":\"0\",";
    s += "\"signatureType\":" + std::to_string(o.signature_type) + ",";
    s += "\"timestamp\":\"" + std::to_string(o.timestamp_ms) + "\",";
    s += "\"metadata\":\"" + o.metadata_hex + "\",";
    s += "\"builder\":\"" + o.builder_hex + "\",";
    s += "\"signature\":\"" + o.signature + "\"},";
    s += "\"owner\":\"" + o.owner + "\",";
    s += "\"orderType\":\"" + o.order_type + "\",";
    s += "\"deferExec\":false,";
    s += "\"postOnly\":false}";
    return s;
}

std::string ComputeL2Signature(std::string_view api_secret_b64url, std::string_view timestamp,
                               std::string_view method, std::string_view path, std::string_view body) {
    const std::string key = Base64UrlDecode(api_secret_b64url);
    std::string base;
    base.reserve(timestamp.size() + method.size() + path.size() + body.size());
    base.append(timestamp).append(method).append(path).append(body);

    unsigned char mac[32];  // HMAC-SHA256 输出 32B
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(base.data()), base.size(), mac, &mac_len);
    return Base64UrlEncode(mac, mac_len);
}

crypto::Bytes32 ComputeClobAuthDigest(std::string_view address_lc, std::string_view timestamp,
                                      std::uint64_t nonce) {
    using crypto::Keccak256;
    // domainSeparator (无 verifyingContract)
    std::uint8_t db[32 * 4];
    std::memcpy(db + 32 * 0, Keccak256("EIP712Domain(string name,string version,uint256 chainId)").data(), 32);
    std::memcpy(db + 32 * 1, Keccak256("ClobAuthDomain").data(), 32);
    std::memcpy(db + 32 * 2, Keccak256("1").data(), 32);
    std::memcpy(db + 32 * 3, crypto::U256FromU64(137).data(), 32);
    const crypto::Bytes32 domain_sep = Keccak256(db, sizeof(db));

    // structHash: ClobAuth(address address,string timestamp,uint256 nonce,string message)
    std::uint8_t sb[32 * 5];
    std::memcpy(sb + 32 * 0,
                Keccak256("ClobAuth(address address,string timestamp,uint256 nonce,string message)").data(), 32);
    std::memcpy(sb + 32 * 1, Addr32(address_lc).data(), 32);
    std::memcpy(sb + 32 * 2, Keccak256(timestamp).data(), 32);
    std::memcpy(sb + 32 * 3, crypto::U256FromU64(nonce).data(), 32);
    std::memcpy(sb + 32 * 4, Keccak256("This message attests that I control the given wallet").data(), 32);
    const crypto::Bytes32 struct_hash = Keccak256(sb, sizeof(sb));

    std::uint8_t pre[2 + 32 + 32];
    pre[0] = 0x19;
    pre[1] = 0x01;
    std::memcpy(pre + 2, domain_sep.data(), 32);
    std::memcpy(pre + 34, struct_hash.data(), 32);
    return Keccak256(pre, sizeof(pre));
}

}  // namespace pmm::wire
