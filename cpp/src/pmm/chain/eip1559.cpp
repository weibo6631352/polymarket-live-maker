// pmm/chain/eip1559.cpp — EIP-1559 交易构建 + 签名实现。
#include "pmm/chain/eip1559.hpp"

#include <cstring>

#include "pmm/chain/rlp.hpp"

namespace pmm::chain {

namespace {

// 9 个未签名字段的 RLP 元素列表 (顺序固定)。
std::vector<rlp::Bytes> UnsignedItems(const Eip1559Tx& tx) {
    std::vector<rlp::Bytes> items;
    items.reserve(12);
    items.push_back(rlp::EncodeUint(tx.chain_id));
    items.push_back(rlp::EncodeUint(tx.nonce));
    items.push_back(rlp::EncodeUint(tx.max_priority_fee_per_gas));
    items.push_back(rlp::EncodeUint(tx.max_fee_per_gas));
    items.push_back(rlp::EncodeUint(tx.gas_limit));
    items.push_back(rlp::EncodeBytes(tx.to.data(), tx.to.size()));  // 20B 地址
    items.push_back(rlp::EncodeUint(tx.value));
    items.push_back(rlp::EncodeBytes(tx.data.data(), tx.data.size()));
    items.push_back(rlp::EncodeList({}));  // accessList = 空 list → 0xc0
    return items;
}

}  // namespace

std::vector<std::uint8_t> EncodeUnsigned(const Eip1559Tx& tx) {
    rlp::Bytes body = rlp::EncodeList(UnsignedItems(tx));
    std::vector<std::uint8_t> out;
    out.reserve(body.size() + 1);
    out.push_back(0x02);  // EIP-2718 type
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

crypto::Bytes32 SigningHash(const Eip1559Tx& tx) {
    const std::vector<std::uint8_t> pre = EncodeUnsigned(tx);
    return crypto::Keccak256(pre.data(), pre.size());
}

bool SignTx(const Eip1559Tx& tx, const crypto::Bytes32& private_key, SignedTx& out) {
    const crypto::Bytes32 digest = SigningHash(tx);
    crypto::Signature65 sig{};
    if (!crypto::SignDigest(digest, private_key, sig)) return false;

    // recover 自检: 恢复地址须 == 私钥派生 EOA (与下单链路同款 P1-5 防线)。
    crypto::Address recovered{};
    crypto::Address derived{};
    if (!crypto::RecoverAddress(digest, sig, recovered)) return false;
    if (!crypto::DeriveAddress(private_key, derived)) return false;
    if (std::memcmp(recovered.data(), derived.data(), 20) != 0) return false;

    out.y_parity = static_cast<std::uint8_t>(sig[64] - 27);  // v = recid+27 → yParity = recid (0/1)

    std::vector<rlp::Bytes> items = UnsignedItems(tx);
    items.push_back(rlp::EncodeUint(out.y_parity));
    items.push_back(rlp::EncodeUintBE(sig.data(), 32));       // r (整数, 去前导零)
    items.push_back(rlp::EncodeUintBE(sig.data() + 32, 32));  // s

    rlp::Bytes body = rlp::EncodeList(items);
    out.raw.clear();
    out.raw.reserve(body.size() + 1);
    out.raw.push_back(0x02);
    out.raw.insert(out.raw.end(), body.begin(), body.end());

    out.tx_hash = crypto::Keccak256(out.raw.data(), out.raw.size());
    return true;
}

}  // namespace pmm::chain
