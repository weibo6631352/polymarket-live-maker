// pmm/chain/ctf.cpp — CTF mergePositions calldata 编码实现。
#include "pmm/chain/ctf.hpp"

#include <cstring>

namespace pmm::chain::ctf {

namespace {

// abi.encode(address) → 32B (左填 12 零)。
crypto::Bytes32 Addr32(const crypto::Address& a) noexcept {
    crypto::Bytes32 out{};
    std::memcpy(out.data() + 12, a.data(), 20);
    return out;
}

void Append32(std::vector<std::uint8_t>& out, const crypto::Bytes32& w) {
    out.insert(out.end(), w.begin(), w.end());
}

}  // namespace

std::array<std::uint8_t, 4> MergeSelector() noexcept {
    static constexpr char kSig[] = "mergePositions(address,bytes32,bytes32,uint256[],uint256)";
    const crypto::Bytes32 h = crypto::Keccak256(std::string_view{kSig, sizeof(kSig) - 1});
    std::array<std::uint8_t, 4> sel{};
    std::memcpy(sel.data(), h.data(), 4);
    return sel;
}

std::vector<std::uint8_t> EncodeMergePositions(const crypto::Address& collateral,
                                               const crypto::Bytes32& parent_collection_id,
                                               const crypto::Bytes32& condition_id,
                                               const std::vector<crypto::Bytes32>& partition,
                                               const crypto::Bytes32& amount) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + 32 * (5 + 1 + partition.size()));

    const std::array<std::uint8_t, 4> sel = MergeSelector();
    out.insert(out.end(), sel.begin(), sel.end());

    // head (5 静态槽): collateral, parent, condition, offset(指向 partition tail), amount。
    // 4 个静态参数 (collateral, parent, condition, amount) + 1 个动态 (partition) → head = 5*32。
    // partition 是第 4 个参数 → 其数据偏移 = head 字节数 = 160 = 0xa0 (与参数个数无关, 长度可变不影响 offset)。
    Append32(out, Addr32(collateral));
    Append32(out, parent_collection_id);
    Append32(out, condition_id);
    Append32(out, crypto::U256FromU64(5ULL * 32ULL));  // offset = 0xa0
    Append32(out, amount);

    // tail: partition (length + 每个 index set)。
    Append32(out, crypto::U256FromU64(static_cast<std::uint64_t>(partition.size())));
    for (const auto& p : partition) Append32(out, p);

    return out;
}

std::vector<std::uint8_t> EncodeMergePositionsBinary(const crypto::Address& collateral,
                                                     const crypto::Bytes32& condition_id,
                                                     std::uint64_t amount) {
    const crypto::Bytes32 parent{};  // 0
    const std::vector<crypto::Bytes32> partition = {crypto::U256FromU64(1), crypto::U256FromU64(2)};
    return EncodeMergePositions(collateral, parent, condition_id, partition, crypto::U256FromU64(amount));
}

}  // namespace pmm::chain::ctf
