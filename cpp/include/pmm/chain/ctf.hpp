// pmm/chain/ctf.hpp — Gnosis Conditional Tokens Framework (CTF) mergePositions calldata 编码。
//
// 机制: 二元市场里 1 YES + 1 NO 永远可经 CTF mergePositions 赎回 $1。Bot 持有一边 (N YES) 时,
//   与其穿薄 YES 盘卖出 (扫盘亏损), 不如买 N 个互补 (NO) 再 mergePositions → 赎回 N USDC。
//   参考: docs.polymarket.com/developers/CTF/merge。
//
// 本模块只做纯 ABI 编码 (selector + 参数), 不签名不发链。selector/布局由 test_merge_encode 对
//   eth_abi + 4byte.directory (9e7212ad) 的逐字节参考验证 (DRY proof)。
//
// 红线: 不碰私钥。R-12: 非 hot path。
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pmm/crypto/eip712_v2.hpp"  // crypto::Address / crypto::Bytes32 / U256FromU64

namespace pmm::chain::ctf {

// Polygon mainnet 合约/资产 (权威源: docs.polymarket.com/developers/CTF)。
inline constexpr std::string_view kCtfAddress = "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045";
// USDC.e (bridged) — 历史抵押。注意: PM 文档近期提到原生 pUSD; 抵押地址是编码参数, 上线前由用户确认。
inline constexpr std::string_view kUsdcE = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";
inline constexpr std::uint64_t kPolygonChainId = 137;

// mergePositions(address,bytes32,bytes32,uint256[],uint256) 的 4 字节 selector。
//   == keccak256(sig)[:4] == 0x9e7212ad (与 ethereum-lists/4bytes 一致, 见单测)。
[[nodiscard]] std::array<std::uint8_t, 4> MergeSelector() noexcept;

// 通用编码: mergePositions(collateral, parentCollectionId, conditionId, partition[], amount)。
//   ABI head = 5 槽 (collateral, parent, condition, offset=0xa0, amount); tail = partition (len + 元素)。
[[nodiscard]] std::vector<std::uint8_t> EncodeMergePositions(
    const crypto::Address& collateral, const crypto::Bytes32& parent_collection_id,
    const crypto::Bytes32& condition_id, const std::vector<crypto::Bytes32>& partition,
    const crypto::Bytes32& amount);

// 二元市场便捷封装: parentCollectionId=0, partition=[1,2], amount 为抵押最小单位 (USDC 6 位小数)。
[[nodiscard]] std::vector<std::uint8_t> EncodeMergePositionsBinary(const crypto::Address& collateral,
                                                                   const crypto::Bytes32& condition_id,
                                                                   std::uint64_t amount);

}  // namespace pmm::chain::ctf
