// pmm/chain/proxy_exec.hpp — Polymarket sig_type=1 (POLY_PROXY) 代理钱包 exec 包装编码。
//
// 背景 (实盘缺口, 见 merge_flatten.hpp): 本账户 POLYMARKET_SIGNATURE_TYPE=1。仓位 (ERC1155) 与
//   USDC 由 funder 代理钱包 0x78dE 持有, 而非签名 EOA 0xb9c8。CTF.mergePositions 销毁"调用者"的
//   token, 故 merge 必须由代理 0x78dE 发起 —— EOA 直发 CTF (sig_type=0 路径) 会销毁不到 token。
//
// 代理架构 (已 DRY 验证, 见 test_merge_encode):
//   - 代理由 Polymarket 的 ProxyWalletFactory (Polygon, 已验证合约, Solidity 0.5.10) 用 CREATE2 部署:
//       proxyAddr = keccak(0xff ++ factory ++ keccak(eoa) ++ initCodeHash)[12:]
//     我们离线证明: factory=0xaB45c5… + initCodeHash=0xd21df8… + EOA 0xb9c8 → 恰好 = funder 0x78dE。
//   - 控制 EOA 直接调用 factory.proxy(ProxyCall[]); 工厂按 msg.sender 路由到该 EOA 的确定性代理,
//     代理再对每个 ProxyCall 发起 CALL/DELEGATECALL → 内层 mergePositions 的 caller = 代理 (烧代理 token)。✓
//   权威源: github.com/Polymarket/proxy-factories (ProxyWalletFactory.sol / ProxyWalletLib.sol),
//     github.com/Polymarket/py-builder-relayer-client (encode/proxy.py — 同一 selector + abi.encode 方案)。
//
// 本模块只做纯 ABI 编码 (selector + 参数), 不签名不发链。selector/布局由 test_merge_encode 对
//   eth-abi 参考 (== py-builder-relayer-client 编码) 逐字节验证。
//
// 红线: 不碰私钥。R-12: 非 hot path (平仓链路)。
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pmm/crypto/eip712_v2.hpp"  // crypto::Address / Bytes32 / U256FromU64

namespace pmm::chain::proxy {

// Polymarket: Proxy Wallet Factory (Polygon mainnet, sig_type=1 POLY_PROXY)。
//   已验证合约 (polygonscan label "Polymarket: Proxy Wallet Factory")。
inline constexpr std::string_view kProxyWalletFactory = "0xaB45c5A4B0c941a2F231C04C3f49182e1A254052";

// ProxyWalletLib.sol 的 CallType 枚举: INVALID=0, CALL=1, DELEGATECALL=2。merge 用 CALL。
enum class CallType : std::uint8_t { kInvalid = 0, kCall = 1, kDelegateCall = 2 };

// 一笔代理子调用 (对应 Solidity struct ProxyCall {uint8 typeCode; address to; uint256 value; bytes data;})。
struct ProxyCall {
    CallType type_code{CallType::kCall};
    crypto::Address to{};
    crypto::Bytes32 value{};             // uint256 (merge = 0)
    std::vector<std::uint8_t> data;      // 内层 calldata (如 mergePositions)
};

// proxy((uint8,address,uint256,bytes)[]) 的 4 字节 selector == 0x34ee9791
//   (== keccak256(sig)[:4], 与 py-builder-relayer-client encode/proxy.py 一致, 见单测)。
[[nodiscard]] std::array<std::uint8_t, 4> ProxySelector() noexcept;

// 编码 factory.proxy(ProxyCall[]) 的完整 calldata (selector + abi.encode((uint8,address,uint256,bytes)[]))。
//   逐字节复刻 Polymarket py-builder-relayer-client 的 encode_proxy_transaction_data。
[[nodiscard]] std::vector<std::uint8_t> EncodeProxy(const std::vector<ProxyCall>& calls);

// 便捷封装: 把单个内层 calldata 包成一笔 CALL (typeCode=1, value=0) → factory.proxy([{CALL, to, 0, inner}])。
[[nodiscard]] std::vector<std::uint8_t> EncodeProxyExec(const crypto::Address& to,
                                                        const std::vector<std::uint8_t>& inner);

}  // namespace pmm::chain::proxy
