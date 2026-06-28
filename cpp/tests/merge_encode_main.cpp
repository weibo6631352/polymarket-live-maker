// tests/merge_encode_main.cpp — DRY 验证: CTF mergePositions calldata + EIP-1559 tx 编码/签名。
//
// gold reference (merge_ref_vectors.hpp) 由独立工具生成 (eth_abi + eth_account + pycryptodome
//   keccak), selector 再与 ethereum-lists/4bytes (9e7212ad) 交叉确认。本测试断言本仓库的编码器/
//   签名器逐字节匹配 → "加密编码正确"的离线证明。绝无任何网络/上链。
//
// 运行: ./build/pmm_merge_smoke  (退出码 0 = 全部通过)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "merge_ref_vectors.hpp"  // 生成的 gold reference (同目录)

#include "pmm/chain/ctf.hpp"
#include "pmm/chain/eip1559.hpp"
#include "pmm/chain/merge_flatten.hpp"
#include "pmm/chain/proxy_exec.hpp"
#include "pmm/chain/rlp.hpp"
#include "pmm/crypto/eip712_v2.hpp"

#include <cstdlib>  // setenv/unsetenv (route-by-sig_type test)

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

std::string Hex(const std::uint8_t* p, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0x0f]);
    }
    return s;
}
std::string Hex(const std::vector<std::uint8_t>& b) { return Hex(b.data(), b.size()); }

}  // namespace

int main() {
    using namespace pmm::chain;
    using pmm::crypto::Address;
    using pmm::crypto::Bytes32;

    // ---- 1) RLP 标准测试向量 ----
    Check(Hex(rlp::EncodeBytes(reinterpret_cast<const std::uint8_t*>("dog"), 3)) == "83646f67",
          "rlp(\"dog\") == 83646f67");
    Check(Hex(rlp::EncodeBytes(nullptr, 0)) == "80", "rlp(\"\") == 80");
    Check(Hex(rlp::EncodeList({})) == "c0", "rlp([]) == c0");
    {
        const auto cat = rlp::EncodeBytes(reinterpret_cast<const std::uint8_t*>("cat"), 3);
        const auto dog = rlp::EncodeBytes(reinterpret_cast<const std::uint8_t*>("dog"), 3);
        Check(Hex(rlp::EncodeList({cat, dog})) == "c88363617483646f67", "rlp([cat,dog])");
    }
    Check(Hex(rlp::EncodeUint(0)) == "80", "rlp_uint(0) == 80");
    Check(Hex(rlp::EncodeUint(15)) == "0f", "rlp_uint(15) == 0f");
    Check(Hex(rlp::EncodeUint(1024)) == "820400", "rlp_uint(1024) == 820400");

    // ---- 2) selector + calldata (vs eth_abi / 4byte.directory) ----
    const auto sel = ctf::MergeSelector();
    Check(Hex(sel.data(), 4) == kRefSelector, "mergePositions selector == 9e7212ad (4byte.directory)");

    Address collateral{};
    Check(pmm::crypto::AddressFromHex(ctf::kUsdcE, collateral), "parse USDC.e address");
    Bytes32 condition{};
    Check(pmm::crypto::Bytes32FromHex(
              "0xabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", condition),
          "parse conditionId");
    const auto calldata = ctf::EncodeMergePositionsBinary(collateral, condition, 1'000'000);
    Check(Hex(calldata) == kRefCalldata, "EncodeMergePositionsBinary == eth_abi reference");
    Check(calldata.size() == 260, "calldata length == 260 bytes");

    {  // 通用编码路径与便捷封装一致
        Bytes32 parent{};
        std::vector<Bytes32> part = {pmm::crypto::U256FromU64(1), pmm::crypto::U256FromU64(2)};
        const auto g = ctf::EncodeMergePositions(collateral, parent, condition, part,
                                                 pmm::crypto::U256FromU64(1'000'000));
        Check(Hex(g) == kRefCalldata, "EncodeMergePositions(general) == binary helper");
    }

    // ---- 3) EIP-1559 tx 构建/签名 (pk = 0x46*32) ----
    Bytes32 pk{};
    for (auto& b : pk) b = 0x46;

    Eip1559Tx base;
    base.chain_id = 137;
    base.max_priority_fee_per_gas = 30'000'000'000ULL;
    base.max_fee_per_gas = 100'000'000'000ULL;
    base.gas_limit = 250'000;
    Check(pmm::crypto::AddressFromHex(ctf::kCtfAddress, base.to), "parse CTF address");
    base.value = 0;
    base.data = calldata;

    {
        Eip1559Tx t = base;
        t.nonce = 7;
        Check(Hex(SigningHash(t).data(), 32) == kRefSigHashNonce7, "SigningHash(nonce=7) matches");
        SignedTx s;
        Check(SignTx(t, pk, s), "SignTx(nonce=7) ok (recover self-check passes)");
        Check(s.y_parity == 0, "nonce=7 yParity == 0");
        Check(Hex(s.raw) == kRefRawNonce7, "raw tx(nonce=7) == eth_account reference");
    }
    {
        Eip1559Tx t = base;
        t.nonce = 0;
        SignedTx s;
        Check(SignTx(t, pk, s) && Hex(s.raw) == kRefRawNonce0, "raw tx(nonce=0) == reference");
    }
    {
        Eip1559Tx t = base;
        t.nonce = 1;
        SignedTx s;
        Check(SignTx(t, pk, s), "SignTx(nonce=1) ok");
        Check(s.y_parity == 1, "nonce=1 yParity == 1");
        Check(Hex(s.raw) == kRefRawNonce1, "raw tx(nonce=1, yParity=1) == reference");
    }

    // ---- 4) MergeExecutor::BuildAndSign 走 CTF + EIP-1559 同一路径 (sig_type=0 默认) ----
    {
        unsetenv("POLYMARKET_SIGNATURE_TYPE");  // 默认 → sig_type=0 (EOA 直发 CTF)
        MergeExecutor exec;                     // 默认双闸 OFF
        MergeQuote q;
        q.collateral = collateral;
        q.condition_id = condition;
        q.amount = 1'000'000;
        SignedTx s;
        Check(exec.BuildAndSign(q, pk, 7, 30'000'000'000ULL, 100'000'000'000ULL, 250'000, s) &&
                  Hex(s.raw) == kRefRawNonce7,
              "MergeExecutor::BuildAndSign(sig0) == EOA-direct reference");
    }

    // ---- 4b) sig_type=1 (POLY_PROXY) 外层包装: proxy((uint8,address,uint256,bytes)[]) ----
    {
        // selector == py-builder-relayer-client encode/proxy.py。
        Check(Hex(proxy::ProxySelector().data(), 4) == kRefProxySelector,
              "proxy() selector == 34ee9791 (py-builder-relayer-client)");

        Address ctf_addr{};
        Check(pmm::crypto::AddressFromHex(ctf::kCtfAddress, ctf_addr), "parse CTF address (proxy.to)");

        // 包裹本测试的 inner merge calldata → 与 eth-abi gold reference 逐字节匹配。
        const auto wrapped = proxy::EncodeProxyExec(ctf_addr, calldata);
        Check(Hex(wrapped) == kRefProxyExecMerge, "EncodeProxyExec(CTF, merge) == eth-abi reference");
        Check(wrapped.size() == 548, "proxy-exec(merge) length == 548 bytes");

        // 跨校验: 用 SDK 自带 fixture (USDC.e approve) 走同一编码器 → 证明与 SDK 编码器逐字节同源。
        Address usdce{};
        (void)pmm::crypto::AddressFromHex(ctf::kUsdcE, usdce);
        std::vector<std::uint8_t> approve;
        {
            const std::string a =
                "095ea7b30000000000000000000000004d97dcd97ec945f40cf65f87097ace5ea0476045"
                "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
            for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
                auto hv = [](char ch) {
                    return ch <= '9' ? ch - '0' : (ch <= 'F' ? ch - 'A' + 10 : ch - 'a' + 10);
                };
                approve.push_back(static_cast<std::uint8_t>((hv(a[i]) << 4) | hv(a[i + 1])));
            }
        }
        Check(Hex(proxy::EncodeProxyExec(usdce, approve)) == kRefProxyExecApprove,
              "EncodeProxyExec(USDC.e, approve) == py-builder-relayer-client fixture");
    }

    // ---- 4c) 路由按 POLYMARKET_SIGNATURE_TYPE 选择 (sig0=CTF 直发, sig1=ProxyWalletFactory) ----
    {
        MergeQuote q;
        q.collateral = collateral;
        q.condition_id = condition;
        q.amount = 1'000'000;

        setenv("POLYMARKET_SIGNATURE_TYPE", "0", 1);
        {
            MergeExecutor exec0;
            const MergeRoute r0 = exec0.RouteFor(q);
            Address ctf_addr{};
            (void)pmm::crypto::AddressFromHex(ctf::kCtfAddress, ctf_addr);
            Check(r0.ok && !r0.via_proxy && Hex(r0.to.data(), 20) == Hex(ctf_addr.data(), 20),
                  "RouteFor(sig0): to == CTF, not via proxy");
            Check(Hex(r0.calldata) == kRefCalldata, "RouteFor(sig0): calldata == inner merge");
        }

        setenv("POLYMARKET_SIGNATURE_TYPE", "1", 1);
        {
            MergeExecutor exec1;
            const MergeRoute r1 = exec1.RouteFor(q);
            Address fac{};
            (void)pmm::crypto::AddressFromHex(proxy::kProxyWalletFactory, fac);
            Check(r1.ok && r1.via_proxy && Hex(r1.to.data(), 20) == Hex(fac.data(), 20),
                  "RouteFor(sig1): to == ProxyWalletFactory, via proxy");
            Check(Hex(r1.calldata) == kRefProxyExecMerge, "RouteFor(sig1): calldata == proxy-wrapped");
            // BuildAndSign 在 sig1 下也成功 (走工厂 to + 包装 data)。
            SignedTx s1;
            Check(exec1.BuildAndSign(q, pk, 7, 30'000'000'000ULL, 100'000'000'000ULL, 250'000, s1),
                  "BuildAndSign(sig1) signs proxy-routed tx ok");
        }

        setenv("POLYMARKET_SIGNATURE_TYPE", "2", 1);
        {
            MergeExecutor exec2;  // GNOSIS_SAFE: 未实现 → ok=false, 不构造任何 tx
            const MergeRoute r2 = exec2.RouteFor(q);
            SignedTx s2;
            Check(!r2.ok && !exec2.BuildAndSign(q, pk, 7, 30'000'000'000ULL, 100'000'000'000ULL,
                                                250'000, s2),
                  "RouteFor(sig2=GNOSIS_SAFE): unsupported → ok=false, BuildAndSign refuses");
        }
        unsetenv("POLYMARKET_SIGNATURE_TYPE");  // 复位, 不影响后续测试
    }

    // ---- 5) 平仓决策 (穿盘卖 vs 买互补+merge) 纯函数 ----
    {
        // 薄 YES bid 盘 (扫盘亏): 0.40×50, 0.30×200。互补 NO ask 便宜: 0.55×500。平 100 股。
        std::vector<BookLevel> yes_bids = {{0.40, 50}, {0.30, 200}};
        std::vector<BookLevel> no_asks = {{0.55, 500}};
        const FlattenDecision d = EvaluateFlatten(100, yes_bids, no_asks, /*gas_usdc=*/0.05);
        // sell: 50*0.40 + 50*0.30 = 35。merge: redeem 100 - buy 100*0.55(=55) - gas 0.05 = 44.95。
        Check(std::abs(d.sell_proceeds_usdc - 35.0) < 1e-9, "decision sell proceeds == 35");
        Check(std::abs(d.merge_net_usdc - 44.95) < 1e-9, "decision merge net == 44.95");
        Check(d.merge_cheaper, "merge cheaper than sweeping thin bid book");

        std::vector<BookLevel> no_asks_pricey = {{0.95, 500}};  // 互补 ask 很贵 → 卖出更优
        const FlattenDecision d2 = EvaluateFlatten(100, yes_bids, no_asks_pricey, 0.05);
        Check(!d2.merge_cheaper, "sweep cheaper when complement ask expensive");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
