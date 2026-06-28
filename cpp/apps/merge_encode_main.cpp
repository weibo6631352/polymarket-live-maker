// apps/merge_encode_main.cpp — DRY 工具: 为一个市场打印 CTF mergePositions 的 calldata + 已签 raw tx。
//
// !! 纯离线: 编码 + 签名 + 打印, 绝不发链 (无 eth_sendRawTransaction)。!! 供用户在监督下的真实
//   平仓测试前审查"将要广播的 tx"。真正上链由 bot 双闸 (LM_FLATTEN_VIA_MERGE + LM_MERGE_ARM_REAL_FUNDS)
//   或用户手工 broadcast 完成 —— 不在本工具内。
//
// 用法:
//   merge-encode <conditionId-0x64hex> <shares> [nonce] [tip_gwei] [maxfee_gwei] [gas]
// 默认用确定性测试私钥 (0x46*32) 演示编码; 设 PM_TRADER_LIVE=1 且 .env 有 POLYMARKET_PRIVATE_KEY
//   时改用真钥, 打印的就是真实可广播 raw (仍不发送)。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pmm/app/dotenv.hpp"
#include "pmm/chain/ctf.hpp"
#include "pmm/chain/eip1559.hpp"
#include "pmm/chain/merge_flatten.hpp"
#include "pmm/crypto/eip712_v2.hpp"
#include "pmm/crypto/secp256k1_signer.hpp"
#include "pmm/env.hpp"

namespace {

std::string Hex(const std::uint8_t* p, std::size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s = "0x";
    for (std::size_t i = 0; i < n; ++i) {
        s += h[p[i] >> 4];
        s += h[p[i] & 0x0f];
    }
    return s;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool HexToBytes(const std::string& hex, std::uint8_t* out, std::size_t n) {
    const char* p = hex.c_str();
    if (hex.size() >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    std::size_t len = 0;
    while (p[len] != '\0') ++len;
    if (len != n * 2) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const int hi = HexVal(p[i * 2]), lo = HexVal(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: merge-encode <conditionId-0x64hex> <shares> [nonce] [tip_gwei] "
                     "[maxfee_gwei] [gas]\n");
        return 2;
    }
    const std::string cond_hex = argv[1];
    const double shares = std::stod(argv[2]);
    const std::uint64_t nonce = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0;
    const std::uint64_t tip_gwei = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 30;
    const std::uint64_t maxfee_gwei = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 100;
    const std::uint64_t gas = argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 250'000;

    pmm::crypto::Bytes32 condition{};
    if (!pmm::crypto::Bytes32FromHex(cond_hex, condition)) {
        std::fprintf(stderr, "bad conditionId (need 0x + 64 hex)\n");
        return 2;
    }
    pmm::crypto::Address collateral{};
    (void)pmm::crypto::AddressFromHex(pmm::chain::ctf::kUsdcE, collateral);

    // amount = shares × 1e6 (USDC 6 位小数 / ERC1155 token 单位)。
    const auto amount = static_cast<std::uint64_t>(shares * 1'000'000.0 + 0.5);

    // 私钥: 默认测试钥; PM_TRADER_LIVE=1 + .env 有真钥 → 用真钥 (仍只签不发)。
    pmm::crypto::Bytes32 pk{};
    bool real_key = false;
    pmm::app::LoadDotEnv(".env", /*live_mode=*/true);
    if (pmm::env::flag_eq("PM_TRADER_LIVE", "1", "0")) {
        const std::string k = pmm::env::str("POLYMARKET_PRIVATE_KEY");
        if (!k.empty() && HexToBytes(k, pk.data(), 32)) real_key = true;
    }
    if (!real_key)
        for (auto& b : pk) b = 0x46;  // 确定性测试钥 (EOA 9d8a62...855a4f)

    pmm::crypto::Address eoa{};
    (void)pmm::crypto::DeriveAddress(pk, eoa);

    // 按 POLYMARKET_SIGNATURE_TYPE 选路由: sig0 → CTF 直发; sig1 → ProxyWalletFactory.proxy(...)。
    pmm::chain::MergeExecutor exec;
    pmm::chain::MergeQuote q;
    q.collateral = collateral;
    q.condition_id = condition;
    q.amount = amount;
    const pmm::chain::MergeRoute route = exec.RouteFor(q);
    if (!route.ok) {
        std::fprintf(stderr, "route not supported: %s\n", route.reason.c_str());
        return 1;
    }
    const auto& data = route.calldata;

    pmm::chain::Eip1559Tx tx;
    tx.chain_id = pmm::chain::ctf::kPolygonChainId;
    tx.nonce = nonce;
    tx.max_priority_fee_per_gas = tip_gwei * 1'000'000'000ULL;
    tx.max_fee_per_gas = maxfee_gwei * 1'000'000'000ULL;
    tx.gas_limit = gas;
    tx.to = route.to;  // sig0=CTF, sig1=ProxyWalletFactory
    tx.value = 0;
    tx.data = data;

    pmm::chain::SignedTx s;
    const bool ok = pmm::chain::SignTx(tx, pk, s);

    std::printf("=== CTF mergePositions DRY encode (NO broadcast) ===\n");
    std::printf("signer EOA   : %s%s\n", Hex(eoa.data(), 20).c_str(),
                real_key ? " (REAL key)" : " (TEST key 0x46*32)");
    std::printf("sig_type     : %d%s\n", exec.SignatureType(),
                route.via_proxy ? " (POLY_PROXY → factory.proxy wrapper)" : " (EOA direct)");
    std::printf("tx.to        : %s%s\n", Hex(route.to.data(), 20).c_str(),
                route.via_proxy ? " (ProxyWalletFactory)" : " (CTF)");
    std::printf("CTF contract : %.*s\n", static_cast<int>(pmm::chain::ctf::kCtfAddress.size()),
                pmm::chain::ctf::kCtfAddress.data());
    std::printf("collateral   : %.*s\n", static_cast<int>(pmm::chain::ctf::kUsdcE.size()),
                pmm::chain::ctf::kUsdcE.data());
    std::printf("conditionId  : %s\n", Hex(condition.data(), 32).c_str());
    std::printf("amount       : %llu (= %.6f sets)\n", static_cast<unsigned long long>(amount),
                static_cast<double>(amount) / 1e6);
    std::printf("nonce/gas    : nonce=%llu tip=%llug maxfee=%llug gas=%llu\n",
                static_cast<unsigned long long>(nonce), static_cast<unsigned long long>(tip_gwei),
                static_cast<unsigned long long>(maxfee_gwei), static_cast<unsigned long long>(gas));
    std::printf("calldata     : %s\n", Hex(data.data(), data.size()).c_str());
    if (!ok) {
        std::printf("SIGN FAILED\n");
        return 1;
    }
    std::printf("raw tx       : %s\n", Hex(s.raw.data(), s.raw.size()).c_str());
    std::printf("tx hash      : %s\n", Hex(s.tx_hash.data(), 32).c_str());
    std::printf("\n(DRY ONLY — this tool never broadcasts. To send: a user-supervised step.)\n");
    return 0;
}
