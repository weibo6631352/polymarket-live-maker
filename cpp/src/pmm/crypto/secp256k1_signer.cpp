// src/pmm/crypto/secp256k1_signer.cpp — Ethereum ECDSA 签名 (Polymarket V2)
//
// Owner: GM (老雷) 2026-05-31。固化自 experiments/laosun-laoli-live-order (实盘验证)。
#include "pmm/crypto/secp256k1_signer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

namespace pmm::crypto {

namespace {

// RAII secp256k1 context (sign+verify), 带侧信道随机化 (小白安全建议)。
class Ctx {
public:
    Ctx() noexcept : ctx_(secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY)) {
        // 随机化 blinding (防侧信道); 失败不致命 (签名仍正确)。
        std::uint8_t seed[32];
        const int fd = ::open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (::read(fd, seed, sizeof(seed)) == static_cast<ssize_t>(sizeof(seed)) && ctx_) {
                // 失败不致命 (签名仍正确); 赋值消费返回值避 gcc -Werror=unused-result ((void) 不抑制)。
                [[maybe_unused]] const int rc = secp256k1_context_randomize(ctx_, seed);
            }
            ::close(fd);
            std::memset(seed, 0, sizeof(seed));
        }
    }
    ~Ctx() {
        if (ctx_) secp256k1_context_destroy(ctx_);
    }
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    secp256k1_context* get() const noexcept { return ctx_; }

private:
    secp256k1_context* ctx_;
};

// uncompressed pubkey(65B, 去 0x04 前缀的 64B) → 20B 地址。
Address PubkeyToAddress(const std::uint8_t pub65[65]) noexcept {
    const Bytes32 h = Keccak256(pub65 + 1, 64);
    Address a;
    std::memcpy(a.data(), h.data() + 12, 20);
    return a;
}

}  // namespace

bool SignDigest(const Bytes32& digest, const Bytes32& private_key, Signature65& out) noexcept {
    const Ctx ctx;
    if (!ctx.get()) return false;
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(ctx.get(), &sig, digest.data(), private_key.data(), nullptr, nullptr))
        return false;
    int recid = 0;
    std::uint8_t compact[64];
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx.get(), compact, &recid, &sig);
    std::memcpy(out.data(), compact, 64);
    out[64] = static_cast<std::uint8_t>(recid + 27);  // Ethereum v
    return true;
}

bool RecoverAddress(const Bytes32& digest, const Signature65& sig, Address& out) noexcept {
    const Ctx ctx;
    if (!ctx.get()) return false;
    const int recid = static_cast<int>(sig[64]) - 27;
    if (recid < 0 || recid > 3) return false;
    secp256k1_ecdsa_recoverable_signature rsig;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx.get(), &rsig, sig.data(), recid))
        return false;
    secp256k1_pubkey pub;
    if (!secp256k1_ecdsa_recover(ctx.get(), &pub, &rsig, digest.data()))
        return false;
    std::uint8_t pub65[65];
    std::size_t publen = 65;
    secp256k1_ec_pubkey_serialize(ctx.get(), pub65, &publen, &pub, SECP256K1_EC_UNCOMPRESSED);
    out = PubkeyToAddress(pub65);
    return true;
}

bool DeriveAddress(const Bytes32& private_key, Address& out) noexcept {
    const Ctx ctx;
    if (!ctx.get()) return false;
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(ctx.get(), &pub, private_key.data()))
        return false;
    std::uint8_t pub65[65];
    std::size_t publen = 65;
    secp256k1_ec_pubkey_serialize(ctx.get(), pub65, &publen, &pub, SECP256K1_EC_UNCOMPRESSED);
    out = PubkeyToAddress(pub65);
    return true;
}

}  // namespace pmm::crypto
