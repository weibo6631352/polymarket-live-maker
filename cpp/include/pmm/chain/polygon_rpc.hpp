// pmm/chain/polygon_rpc.hpp — 极简 Polygon JSON-RPC 客户端 (libcurl POST)。
//
// Bot 此前无任何 eth_* / JSON-RPC 能力 (链上持仓只走 Polymarket data-api 只读)。本客户端补齐
//   nonce 查询 / gas 估算 / 发送 raw tx —— mergePositions 平仓所需的最小一组调用。
//
// 安全: SendRawTransaction 是唯一会改变链上状态 (花真钱) 的方法。它本身只是客户端;
//   是否调用由 MergeExecutor 的双闸 (LM_FLATTEN_VIA_MERGE + LM_MERGE_ARM_REAL_FUNDS) 决定, 默认全 OFF。
//   本仓库的构建/测试/DRY 工具绝不调用它。
//
// 红线: 不持有私钥 (只收已签好的 raw bytes)。R-12: 非 hot path。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pmm::chain {

struct RpcResult {
    bool ok{false};
    std::string value;  // 成功: result 字段 (字符串/hex); 失败: error 文本
    int http{0};
};

class PolygonRpc {
public:
    // url 例: https://polygon-rpc.com  (从 env POLYGON_RPC_URL 取; 不写死带 key 的私有节点)。
    explicit PolygonRpc(std::string url, int timeout_ms = 5000);
    ~PolygonRpc();
    PolygonRpc(const PolygonRpc&) = delete;
    PolygonRpc& operator=(const PolygonRpc&) = delete;

    // eth_chainId → 137 (确认连对链)。失败 nullopt。
    [[nodiscard]] std::optional<std::uint64_t> ChainId();
    // eth_getTransactionCount(address, "pending") → 下一个 nonce。
    [[nodiscard]] std::optional<std::uint64_t> TransactionCount(const std::string& address_hex);
    // eth_maxPriorityFeePerGas → 建议小费 (wei)。
    [[nodiscard]] std::optional<std::uint64_t> MaxPriorityFeePerGas();
    // eth_gasPrice → 当前 gas price (wei); 配合小费推 maxFee。
    [[nodiscard]] std::optional<std::uint64_t> GasPrice();
    // eth_estimateGas({from,to,data,value}) → gas 上限估算。
    [[nodiscard]] std::optional<std::uint64_t> EstimateGas(const std::string& from_hex,
                                                           const std::string& to_addr,
                                                           const std::vector<std::uint8_t>& data,
                                                           std::uint64_t value = 0);

    // eth_sendRawTransaction(0x..raw) → tx hash。!! 真实上链, 花真钱 !!
    //   只应由 MergeExecutor 在双闸全开 + 用户监督下调用。返回 tx hash 或 error。
    [[nodiscard]] RpcResult SendRawTransaction(const std::vector<std::uint8_t>& raw);

private:
    RpcResult Call(const std::string& method, const std::string& params_json);
    std::string url_;
    int timeout_ms_;
    void* curl_{nullptr};  // 持久 keep-alive handle (void* 避免头引 curl.h)
};

}  // namespace pmm::chain
