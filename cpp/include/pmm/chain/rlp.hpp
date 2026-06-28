// pmm/chain/rlp.hpp — 最小 RLP 编码器 (Ethereum 交易序列化)。
//
// 为 EIP-1559 (type-2) 交易构建提供 RLP string/list/uint 编码。纯函数, 无外部依赖, 可离线对齐
// (标准 RLP 测试向量 + 真实签名 tx 逐字节匹配, 见 test_merge_encode)。
//
// 红线: 不碰私钥, 不发网络。R-12: 非 hot path (平仓链路, 非 WSS event loop)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pmm::chain::rlp {

using Bytes = std::vector<std::uint8_t>;

// 把一段原始字节编码为 RLP string item。
//   len==1 且 byte<0x80 → 单字节本身; len<=55 → 0x80+len, bytes; 否则 0xb7+len(len), be(len), bytes。
[[nodiscard]] Bytes EncodeBytes(const std::uint8_t* data, std::size_t len);
[[nodiscard]] inline Bytes EncodeBytes(const Bytes& b) { return EncodeBytes(b.data(), b.size()); }

// 把非负整数按以太坊惯例编码 (最小 big-endian, 0 → 空串 0x80)。
[[nodiscard]] Bytes EncodeUint(std::uint64_t v);

// 把一个大端字节序列当作整数编码 (去前导零再当 string; 全零 → 空串)。用于 r/s/大额 fee。
[[nodiscard]] Bytes EncodeUintBE(const std::uint8_t* be, std::size_t len);

// 把"已各自 RLP 编码好的元素"拼成一个 RLP list。
//   payload<=55 → 0xc0+len, payload; 否则 0xf7+len(len), be(len), payload。
[[nodiscard]] Bytes EncodeList(const std::vector<Bytes>& items);

}  // namespace pmm::chain::rlp
