// src/pmm/crypto/eip712_v2.cpp — Polymarket CTF Exchange V2 EIP-712 编码 (生产)
//
// Owner: GM (老雷) — 2026-05-31。固化自 experiments/laosun-laoli-live-order。
// digest 已与官方 eth_account (py-clob-client-v2 路径) 逐字节对齐 (见 test_eip712_v2)。
#include "pmm/crypto/eip712_v2.hpp"

#include <cstring>

namespace pmm::crypto {

namespace {

// ---- Keccak-f[1600] (24 轮) ----
constexpr std::uint64_t Rotl64(std::uint64_t x, unsigned n) noexcept {
    return (x << n) | (x >> (64 - n));
}

void KeccakF(std::uint64_t st[25]) noexcept {
    static constexpr std::uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
        0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
    static constexpr unsigned ROT[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                         27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
    static constexpr unsigned PIL[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                                         15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

    for (int round = 0; round < 24; ++round) {
        std::uint64_t bc[5];
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            const std::uint64_t t = bc[(i + 4) % 5] ^ Rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }
        std::uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            const int j = static_cast<int>(PIL[i]);
            const std::uint64_t tmp = st[j];
            st[j] = Rotl64(t, ROT[i]);
            t = tmp;
        }
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i)
                bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i)
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        st[0] ^= RC[round];
    }
}

int HexVal(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string_view StripZx(std::string_view h) noexcept {
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X'))
        return h.substr(2);
    return h;
}

bool HexToBytes(std::string_view hex, std::uint8_t* out, std::size_t n) noexcept {
    hex = StripZx(hex);
    if (hex.size() != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = HexVal(hex[i * 2]);
        const int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// abi.encode(address) → 32B (左填 12 零)。
void AddrTo32(const Address& a, std::uint8_t* out) noexcept {
    std::memset(out, 0, 32);
    std::memcpy(out + 12, a.data(), 20);
}

}  // namespace

Bytes32 Keccak256(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint64_t st[25];
    std::memset(st, 0, sizeof(st));
    constexpr std::size_t kRate = 136;  // keccak256: 1088 bits
    auto* sb = reinterpret_cast<std::uint8_t*>(st);

    std::size_t i = 0;
    while (len >= kRate) {
        for (std::size_t k = 0; k < kRate; ++k)
            sb[k] ^= data[i + k];
        KeccakF(st);
        i += kRate;
        len -= kRate;
    }
    std::uint8_t tmp[kRate];
    std::memset(tmp, 0, kRate);
    if (len) std::memcpy(tmp, data + i, len);
    tmp[len] ^= 0x01;          // Ethereum padding
    tmp[kRate - 1] ^= 0x80;
    for (std::size_t k = 0; k < kRate; ++k)
        sb[k] ^= tmp[k];
    KeccakF(st);

    Bytes32 out;
    std::memcpy(out.data(), sb, 32);
    return out;
}

Bytes32 U256FromU64(std::uint64_t v) noexcept {
    Bytes32 out{};
    for (int i = 0; i < 8; ++i)
        out[31 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    return out;
}

bool U256FromDecimal(std::string_view dec, Bytes32& out) noexcept {
    out = Bytes32{};
    if (dec.empty()) return false;
    for (char c : dec) {
        if (c < '0' || c > '9') return false;
        unsigned carry = static_cast<unsigned>(c - '0');
        for (int i = 31; i >= 0; --i) {
            const unsigned x = static_cast<unsigned>(out[static_cast<std::size_t>(i)]) * 10 + carry;
            out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x & 0xff);
            carry = x >> 8;
        }
        if (carry) return false;  // 溢出 256 bit
    }
    return true;
}

bool AddressFromHex(std::string_view hex, Address& out) noexcept {
    return HexToBytes(hex, out.data(), 20);
}

bool Bytes32FromHex(std::string_view hex, Bytes32& out) noexcept {
    return HexToBytes(hex, out.data(), 32);
}

Eip712Domain CtfExchangeV2Domain(bool neg_risk) noexcept {
    Eip712Domain d;
    d.name = "Polymarket CTF Exchange";
    d.version = "2";
    d.chain_id = 137;
    // py-clob-client-v2 config.py: exchange_v2 / neg_risk_exchange_v2 (硬编码合法字面量)
    (void)AddressFromHex(neg_risk ? "0xe2222d279d744050d28e00520010520000310F59"
                                  : "0xE111180000d2663C0091e4f400237545B87B996B",
                         d.verifying_contract);
    return d;
}

Bytes32 ComputeOrderV2Digest(const OrderV2& order, const Eip712Domain& domain) noexcept {
    // ---- structHash: keccak(typeHash || 11×32B 字段) ----
    static constexpr char kOrderType[] =
        "Order(uint256 salt,address maker,address signer,uint256 tokenId,uint256 makerAmount,"
        "uint256 takerAmount,uint8 side,uint8 signatureType,uint256 timestamp,"
        "bytes32 metadata,bytes32 builder)";
    const Bytes32 type_hash = Keccak256(std::string_view{kOrderType, sizeof(kOrderType) - 1});

    std::uint8_t sbuf[32 * 12];
    std::memcpy(sbuf + 32 * 0, type_hash.data(), 32);
    std::memcpy(sbuf + 32 * 1, order.salt.data(), 32);
    AddrTo32(order.maker, sbuf + 32 * 2);
    AddrTo32(order.signer, sbuf + 32 * 3);
    std::memcpy(sbuf + 32 * 4, order.token_id.data(), 32);
    std::memcpy(sbuf + 32 * 5, U256FromU64(order.maker_amount).data(), 32);
    std::memcpy(sbuf + 32 * 6, U256FromU64(order.taker_amount).data(), 32);
    std::memcpy(sbuf + 32 * 7, U256FromU64(order.side).data(), 32);
    std::memcpy(sbuf + 32 * 8, U256FromU64(order.signature_type).data(), 32);
    std::memcpy(sbuf + 32 * 9, U256FromU64(order.timestamp_ms).data(), 32);
    std::memcpy(sbuf + 32 * 10, order.metadata.data(), 32);
    std::memcpy(sbuf + 32 * 11, order.builder.data(), 32);
    const Bytes32 struct_hash = Keccak256(sbuf, sizeof(sbuf));

    // ---- domainSeparator: keccak(domTypeHash || keccak(name) || keccak(version) || chainId || contract) ----
    static constexpr char kDomainType[] =
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    std::uint8_t dbuf[32 * 5];
    std::memcpy(dbuf + 32 * 0, Keccak256(std::string_view{kDomainType, sizeof(kDomainType) - 1}).data(), 32);
    std::memcpy(dbuf + 32 * 1, Keccak256(domain.name).data(), 32);
    std::memcpy(dbuf + 32 * 2, Keccak256(domain.version).data(), 32);
    std::memcpy(dbuf + 32 * 3, U256FromU64(domain.chain_id).data(), 32);
    AddrTo32(domain.verifying_contract, dbuf + 32 * 4);
    const Bytes32 domain_sep = Keccak256(dbuf, sizeof(dbuf));

    // ---- digest = keccak(0x19 0x01 || domainSep || structHash) ----
    std::uint8_t pre[2 + 32 + 32];
    pre[0] = 0x19;
    pre[1] = 0x01;
    std::memcpy(pre + 2, domain_sep.data(), 32);
    std::memcpy(pre + 34, struct_hash.data(), 32);
    return Keccak256(pre, sizeof(pre));
}

}  // namespace pmm::crypto
