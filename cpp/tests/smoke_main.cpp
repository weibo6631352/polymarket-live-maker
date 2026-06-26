// apps/smoke_main.cpp — 脚手架自检: 验证从 sports-trader-cpp 复用的签名基建在新工程里
//   编译 + 链接 + 数值正确 (keccak256 测试向量 + secp256k1 经典测试私钥地址 + 签名/恢复往返 +
//   EIP-712 order digest + CLOB wire body 构造)。这是 C++ 化的第一个绿色检查点。
//
// 运行: ./build/pmm_smoke   (退出码 0 = 全部通过)
#include "pmm/crypto/eip712_v2.hpp"
#include "pmm/crypto/secp256k1_signer.hpp"
#include "pmm/wire/clob_wire.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

std::string ToHex(const std::uint8_t* p, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0x0f]);
    }
    return s;
}

int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

}  // namespace

int main() {
    using namespace pmm::crypto;

    // 1) keccak256("") 已知测试向量 (Ethereum Keccak, padding 0x01)。
    const Bytes32 k = Keccak256(std::string_view{""});
    Check(ToHex(k.data(), 32) == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
          "keccak256(\"\") == c5d2460186...85a470");

    // 2) EIP-155 经典测试私钥 0x46*32 → EOA 0x9d8A62...855A4F。
    Bytes32 pk{};
    for (auto& b : pk) b = 0x46;
    Address addr{};
    Check(DeriveAddress(pk, addr), "DeriveAddress(pk) ok");
    Check(ToHex(addr.data(), 20) == "9d8a62f656a8d1615c1294fd71e9cfb3e4855a4f",
          "EOA == 9d8a62f656a8d1615c1294fd71e9cfb3e4855a4f");

    // 3) 可恢复签名往返: sign(digest) → recover == signer。
    Signature65 sig{};
    Check(SignDigest(k, pk, sig), "SignDigest ok");
    Address rec{};
    Check(RecoverAddress(k, sig, rec) && std::memcmp(rec.data(), addr.data(), 20) == 0,
          "RecoverAddress == signer EOA");

    // 4) EIP-712 V2 order digest 跑通 (CTF Exchange V2 domain)。
    OrderV2 o{};
    o.maker = addr;
    o.signer = addr;
    o.maker_amount = 1'000'000;
    o.taker_amount = 2'000'000;
    const Eip712Domain dom = CtfExchangeV2Domain(/*neg_risk=*/false);
    const Bytes32 order_digest = ComputeOrderV2Digest(o, dom);
    Check(order_digest != Bytes32{}, "ComputeOrderV2Digest non-zero");

    // 5) CLOB V2 wire body 构造 + base64url 往返。
    pmm::wire::OrderV2Wire w;
    w.salt = 1;
    w.maker = "0x" + ToHex(addr.data(), 20);
    w.signer = w.maker;
    w.token_id = "123456789";
    const std::string body = pmm::wire::BuildOrderV2Body(w);
    Check(!body.empty() && body.front() == '{', "BuildOrderV2Body produces JSON");
    const std::string enc = pmm::wire::Base64UrlEncode(k.data(), 32);
    const std::string dec = pmm::wire::Base64UrlDecode(enc);
    Check(dec.size() == 32 && std::memcmp(dec.data(), k.data(), 32) == 0, "base64url round-trip");

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
