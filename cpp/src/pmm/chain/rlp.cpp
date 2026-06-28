// pmm/chain/rlp.cpp — 最小 RLP 编码器实现。
#include "pmm/chain/rlp.hpp"

namespace pmm::chain::rlp {

namespace {

// 把长度 n 编码为最小 big-endian 字节 (n==0 → 空)。
Bytes MinBE(std::uint64_t n) {
    Bytes out;
    std::uint8_t buf[8];
    int i = 8;
    while (n != 0) {
        buf[--i] = static_cast<std::uint8_t>(n & 0xff);
        n >>= 8;
    }
    out.assign(buf + i, buf + 8);
    return out;
}

// string/list 的长度前缀: short_tag=0x80/0xc0, long_tag=0xb7/0xf7。
void AppendLenPrefix(Bytes& out, std::size_t len, std::uint8_t short_tag, std::uint8_t long_tag) {
    if (len <= 55) {
        out.push_back(static_cast<std::uint8_t>(short_tag + len));
    } else {
        const Bytes be = MinBE(static_cast<std::uint64_t>(len));
        out.push_back(static_cast<std::uint8_t>(long_tag + be.size()));
        out.insert(out.end(), be.begin(), be.end());
    }
}

}  // namespace

Bytes EncodeBytes(const std::uint8_t* data, std::size_t len) {
    Bytes out;
    if (len == 1 && data[0] < 0x80) {
        out.push_back(data[0]);  // 单字节自表示
        return out;
    }
    AppendLenPrefix(out, len, 0x80, 0xb7);
    out.insert(out.end(), data, data + len);
    return out;
}

Bytes EncodeUint(std::uint64_t v) {
    const Bytes be = MinBE(v);  // v==0 → 空 → EncodeBytes 给 0x80
    return EncodeBytes(be.data(), be.size());
}

Bytes EncodeUintBE(const std::uint8_t* be, std::size_t len) {
    std::size_t i = 0;
    while (i < len && be[i] == 0) ++i;  // 去前导零 (整数语义)
    return EncodeBytes(be + i, len - i);
}

Bytes EncodeList(const std::vector<Bytes>& items) {
    Bytes payload;
    for (const auto& it : items) payload.insert(payload.end(), it.begin(), it.end());
    Bytes out;
    AppendLenPrefix(out, payload.size(), 0xc0, 0xf7);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace pmm::chain::rlp
