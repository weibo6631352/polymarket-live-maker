// pmm/chain/proxy_exec.cpp — Polymarket sig_type=1 代理 exec 包装编码实现。
#include "pmm/chain/proxy_exec.hpp"

#include <cstring>

namespace pmm::chain::proxy {

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

std::array<std::uint8_t, 4> ProxySelector() noexcept {
    static constexpr char kSig[] = "proxy((uint8,address,uint256,bytes)[])";
    const crypto::Bytes32 h = crypto::Keccak256(std::string_view{kSig, sizeof(kSig) - 1});
    std::array<std::uint8_t, 4> sel{};
    std::memcpy(sel.data(), h.data(), 4);
    return sel;
}

std::vector<std::uint8_t> EncodeProxy(const std::vector<ProxyCall>& calls) {
    const std::array<std::uint8_t, 4> sel = ProxySelector();
    std::vector<std::uint8_t> out(sel.begin(), sel.end());

    // 唯一参数是动态类型 (动态数组) → head 仅一槽: 指向 arg0 数据的偏移 = 0x20。
    Append32(out, crypto::U256FromU64(0x20));

    // ---- arg0: (uint8,address,uint256,bytes)[] ----
    // 元素 tuple 含 bytes → tuple 为动态; 动态数组+动态元素的编码:
    //   len ++ head(off[0..n-1]) ++ tail(elem[0..n-1]), off[i] 相对"len 之后"起点。
    const std::uint64_t n = static_cast<std::uint64_t>(calls.size());

    // 先各自编码每个 tuple 元素 (tail)。
    std::vector<std::vector<std::uint8_t>> elems;
    elems.reserve(calls.size());
    for (const ProxyCall& c : calls) {
        std::vector<std::uint8_t> e;
        // tuple head (4 槽): typeCode(uint8), to(address), value(uint256), offsetToData=0x80。
        Append32(e, crypto::U256FromU64(static_cast<std::uint64_t>(c.type_code)));
        Append32(e, Addr32(c.to));
        Append32(e, c.value);
        Append32(e, crypto::U256FromU64(0x80));
        // tuple tail: bytes = len + data (右填 0 到 32 字节边界)。
        Append32(e, crypto::U256FromU64(static_cast<std::uint64_t>(c.data.size())));
        e.insert(e.end(), c.data.begin(), c.data.end());
        if (const std::size_t rem = c.data.size() % 32; rem != 0)
            e.insert(e.end(), 32 - rem, std::uint8_t{0});
        elems.push_back(std::move(e));
    }

    // 数组体: length, 然后 n 个偏移 (head), 然后各元素 (tail)。
    Append32(out, crypto::U256FromU64(n));
    std::uint64_t offset = n * 32ULL;  // head 区大小 = n 槽
    std::vector<std::uint8_t> bodies;
    for (const std::vector<std::uint8_t>& e : elems) {
        Append32(out, crypto::U256FromU64(offset));
        offset += e.size();
        bodies.insert(bodies.end(), e.begin(), e.end());
    }
    out.insert(out.end(), bodies.begin(), bodies.end());
    return out;
}

std::vector<std::uint8_t> EncodeProxyExec(const crypto::Address& to,
                                          const std::vector<std::uint8_t>& inner) {
    ProxyCall c;
    c.type_code = CallType::kCall;  // 标准 CALL (非 delegatecall)
    c.to = to;
    c.value = crypto::Bytes32{};  // 0
    c.data = inner;
    return EncodeProxy({c});
}

}  // namespace pmm::chain::proxy
